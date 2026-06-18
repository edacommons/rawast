// Generic preprocessor expression evaluator. Walks the documented
// expression-AST shape (see grammars/sv_pp_expr.rawast) without any
// SV- or grammar-specific knowledge — any preprocessor whose `\`if`
// condition rule emits this shape can plug in by setting
// PpOptions::expr_eval to a binding of default_pp_expr_eval(...).

#include <rawast/preprocessor.hpp>
#include <rawast/grammar.hpp>
#include <rawast/stream.hpp>

#include <charconv>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

namespace rawast {

namespace {

using RefResolver =
    std::function<std::optional<std::string>(const std::string&)>;

// `as_dict` / `as_array` / `as_string` / `as_int` are in value.hpp.

inline ValuePtr dict_get(const DictValue& d, const char* key) {
    auto it = d.data().find(key);
    return it == d.data().end() ? ValuePtr{} : it->second;
}

inline std::optional<std::string> dict_get_str(const DictValue& d, const char* key) {
    auto sv = as_string(dict_get(d, key));
    if (!sv) return std::nullopt;
    return sv->data();
}

// Parse `text` as a signed decimal integer. Returns nullopt if the
// text is empty, has non-digit characters, or overflows int64.
std::optional<std::int64_t> parse_int(const std::string& text) {
    if (text.empty()) return std::nullopt;
    std::int64_t out = 0;
    const char* first = text.data();
    const char* last  = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    if (ec != std::errc{} || ptr != last) return std::nullopt;
    return out;
}

// Forward declarations: int and bool eval entries call each other
// (e.g. `(A + 1) != 0` evaluates the inner sum as int then compares).
std::optional<std::int64_t>
eval_int(const ValuePtr& v, const RefResolver& resolver);

std::optional<bool>
eval_bool(const ValuePtr& v, const RefResolver& resolver);

// Try to resolve `name` to an int via the host's ref_resolver: the
// macro body string is parsed as int. Returns nullopt if undefined
// or if the body doesn't parse as a base-10 int.
std::optional<std::int64_t>
resolve_ref_as_int(const std::string& name, const RefResolver& resolver) {
    if (!resolver) return std::nullopt;
    auto body = resolver(name);
    if (!body) return std::nullopt;
    return parse_int(*body);
}

// `defined(X)` recognises X by its `value` field (the ref's identifier).
std::optional<bool>
eval_defined(const std::shared_ptr<ArrayValue>& args, const RefResolver& resolver) {
    if (!args || args->data().empty()) return std::nullopt;
    auto arg0 = as_dict(args->data()[0]);
    if (!arg0) return std::nullopt;
    auto name = dict_get_str(*arg0, "value");
    if (!name) return std::nullopt;
    if (!resolver) return std::nullopt;
    return resolver(*name).has_value();
}

std::optional<std::int64_t>
eval_int(const ValuePtr& v, const RefResolver& resolver) {
    auto d = as_dict(v);
    if (!d) return std::nullopt;

    // Operator node?
    if (auto op_opt = dict_get_str(*d, "op")) {
        const std::string& op = *op_opt;
        auto args = as_array(dict_get(*d, "args"));
        if (!args || args->data().empty()) return std::nullopt;

        // Unary `!` flips truthiness — surface as int 0/1.
        if (op == "!") {
            auto b = eval_bool(args->data()[0], resolver);
            if (!b) return std::nullopt;
            return *b ? 0 : 1;
        }

        // Boolean ops short-circuit on bool semantics; cast the bool
        // result back to int so callers in int context get 0/1.
        if (op == "||" || op == "&&") {
            auto b = eval_bool(v, resolver);
            if (!b) return std::nullopt;
            return *b ? 1 : 0;
        }

        // Comparisons evaluate args as int, return 0/1.
        if (op == "==" || op == "!=" || op == "<" || op == ">"
            || op == "<=" || op == ">=") {
            if (args->data().size() < 2) return std::nullopt;
            auto a = eval_int(args->data()[0], resolver);
            auto b = eval_int(args->data()[1], resolver);
            if (!a || !b) return std::nullopt;
            if (op == "==") return *a == *b ? 1 : 0;
            if (op == "!=") return *a != *b ? 1 : 0;
            if (op == "<")  return *a <  *b ? 1 : 0;
            if (op == ">")  return *a >  *b ? 1 : 0;
            if (op == "<=") return *a <= *b ? 1 : 0;
            return *a >= *b ? 1 : 0;
        }

        // Arithmetic: fold args left-to-right.
        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
            auto first = eval_int(args->data()[0], resolver);
            if (!first) return std::nullopt;
            std::int64_t acc = *first;
            for (std::size_t i = 1; i < args->data().size(); ++i) {
                auto next = eval_int(args->data()[i], resolver);
                if (!next) return std::nullopt;
                if (op == "+") acc += *next;
                else if (op == "-") acc -= *next;
                else if (op == "*") acc *= *next;
                else {
                    if (*next == 0) return std::nullopt;  // div/mod by zero
                    if (op == "/") acc /= *next;
                    else           acc %= *next;
                }
            }
            return acc;
        }
        return std::nullopt;
    }

    // Leaf type?
    if (auto type_opt = dict_get_str(*d, "type")) {
        const std::string& type = *type_opt;
        if (type == "int") {
            auto iv = as_int(dict_get(*d, "value"));
            if (!iv) return std::nullopt;
            return iv->data();
        }
        if (type == "paren") {
            return eval_int(dict_get(*d, "value"), resolver);
        }
        if (type == "ref") {
            auto name = dict_get_str(*d, "value");
            if (!name) return std::nullopt;
            return resolve_ref_as_int(*name, resolver);
        }
        if (type == "call") {
            auto name = dict_get_str(*d, "name");
            if (!name) return std::nullopt;
            if (*name == "defined") {
                auto b = eval_defined(as_array(dict_get(*d, "args")), resolver);
                if (!b) return std::nullopt;
                return *b ? 1 : 0;
            }
            // Other call names: host's responsibility — undecidable here.
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<bool>
eval_bool(const ValuePtr& v, const RefResolver& resolver) {
    auto d = as_dict(v);
    if (!d) return std::nullopt;

    // Operator node?
    if (auto op_opt = dict_get_str(*d, "op")) {
        const std::string& op = *op_opt;
        auto args = as_array(dict_get(*d, "args"));
        if (!args || args->data().empty()) return std::nullopt;

        if (op == "!") {
            auto b = eval_bool(args->data()[0], resolver);
            if (!b) return std::nullopt;
            return !*b;
        }
        if (op == "||") {
            // Short-circuit on the first true; nullopt only if no true
            // was found and at least one arg was undecidable.
            bool any_unknown = false;
            for (const auto& a : args->data()) {
                auto r = eval_bool(a, resolver);
                if (!r) { any_unknown = true; continue; }
                if (*r) return true;
            }
            if (any_unknown) return std::nullopt;
            return false;
        }
        if (op == "&&") {
            // Short-circuit on first false; nullopt only if no false
            // was found and at least one arg was undecidable.
            bool any_unknown = false;
            for (const auto& a : args->data()) {
                auto r = eval_bool(a, resolver);
                if (!r) { any_unknown = true; continue; }
                if (!*r) return false;
            }
            if (any_unknown) return std::nullopt;
            return true;
        }
        // Comparisons / arithmetic → int, then truthiness.
        auto n = eval_int(v, resolver);
        if (!n) return std::nullopt;
        return *n != 0;
    }

    // Leaf type?
    if (auto type_opt = dict_get_str(*d, "type")) {
        const std::string& type = *type_opt;
        if (type == "int") {
            auto iv = as_int(dict_get(*d, "value"));
            if (!iv) return std::nullopt;
            return iv->data() != 0;
        }
        if (type == "paren") {
            return eval_bool(dict_get(*d, "value"), resolver);
        }
        if (type == "ref") {
            // Defined-as-int with body 0 → falsy. Defined otherwise → truthy.
            // Undefined → false (the standard C-preprocessor semantic for
            // unknown identifiers in `\`if` is to substitute zero).
            auto name = dict_get_str(*d, "value");
            if (!name) return std::nullopt;
            if (!resolver) return std::nullopt;
            auto body = resolver(*name);
            if (!body) return false;
            if (auto n = parse_int(*body)) return *n != 0;
            return true;   // defined, body not int → truthy
        }
        if (type == "call") {
            auto name = dict_get_str(*d, "name");
            if (!name) return std::nullopt;
            if (*name == "defined") {
                return eval_defined(as_array(dict_get(*d, "args")), resolver);
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<bool> default_pp_expr_eval(
    const ValuePtr& cond,
    const std::function<std::optional<std::string>(const std::string&)>& ref_resolver) {
    return eval_bool(cond, ref_resolver);
}

void Preprocessor::use_default_expr_eval() {
    opts_.expr_eval = [this](const ValuePtr& cond) -> std::optional<bool> {
        return default_pp_expr_eval(cond,
            [this](const std::string& name) -> std::optional<std::string> {
                if (auto m = get_macro(name)) {
                    // The resolver needs a string view of the body
                    // (the default expr-eval parses it as an int
                    // in arithmetic context). Render the segments
                    // back to text — no expansion, just leaf
                    // representation.
                    if (!m->body_segments) return std::nullopt;
                    std::string s;
                    for (auto& seg : m->body_segments->data()) {
                        if (auto sv = std::dynamic_pointer_cast<StringValue>(seg)) {
                            s += sv->data();
                        }
                    }
                    return s;
                }
                return std::nullopt;
            });
    };
}

void Preprocessor::use_default_expr_eval(const Grammar& expr_grammar) {
    opts_.expr_eval = [this, &expr_grammar](const ValuePtr& cond)
        -> std::optional<bool> {
        auto resolver =
            [this](const std::string& name) -> std::optional<std::string> {
                if (auto m = get_macro(name)) {
                    // The resolver needs a string view of the body
                    // (the default expr-eval parses it as an int
                    // in arithmetic context). Render the segments
                    // back to text — no expansion, just leaf
                    // representation.
                    if (!m->body_segments) return std::nullopt;
                    std::string s;
                    for (auto& seg : m->body_segments->data()) {
                        if (auto sv = std::dynamic_pointer_cast<StringValue>(seg)) {
                            s += sv->data();
                        }
                    }
                    return s;
                }
                return std::nullopt;
            };
        // Synthesized AST cond (process_ast path or other grammar
        // shapes) goes straight to the evaluator — no parse needed.
        if (!as_string(cond)) {
            return default_pp_expr_eval(cond, resolver);
        }
        // Raw-text cond from sv_preprocessor.rawast's IF rule: parse
        // it with the supplied expression grammar.
        auto stream = Stream::from_string(as_string(cond)->data());
        auto r = expr_grammar.parse(stream);
        if (!r) {
            state_.warnings.push_back(
                {"failed to parse `if condition '" +
                     as_string(cond)->data() + "': " + r.error().message,
                 state_.current_file, state_.current_line});
            return std::nullopt;
        }
        return default_pp_expr_eval(*r, resolver);
    };
}

} // namespace rawast
