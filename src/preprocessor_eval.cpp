// Generic preprocessor expression evaluator. Walks the documented
// expression-AST shape (the `\`if-cond rules in systemverilog.rawast)
// without any
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

// Evaluate a sized/based SV literal node (`type:"based_num"`, e.g.
// `8'hFF`, `4'sd3`, `1'b1`). Fields: optional `size` (int), `base`
// (b/o/d/h), `value` (digit string, may carry `_`), optional `signed`.
// Returns nullopt for indeterminate literals (any x/z/? don't-care
// digit) — those can't fold to a constant in a `\`if.
std::optional<std::int64_t> eval_based_num(const DictValue& d) {
    auto base = dict_get_str(d, "base");
    auto valstr = dict_get_str(d, "value");
    if (!base || base->empty() || !valstr) return std::nullopt;
    int radix;
    switch (base->front() | 0x20) {  // tolower
        case 'h': radix = 16; break;
        case 'd': radix = 10; break;
        case 'o': radix = 8;  break;
        case 'b': radix = 2;  break;
        default: return std::nullopt;
    }
    std::uint64_t raw = 0;
    for (char c : *valstr) {
        if (c == '_') continue;
        char lc = static_cast<char>(c | 0x20);
        if (lc == 'x' || lc == 'z' || c == '?') return std::nullopt;
        int dv;
        if (c >= '0' && c <= '9')        dv = c - '0';
        else if (lc >= 'a' && lc <= 'f') dv = 10 + (lc - 'a');
        else return std::nullopt;
        if (dv >= radix) return std::nullopt;
        raw = raw * static_cast<std::uint64_t>(radix) + static_cast<std::uint64_t>(dv);
    }
    // Width truncation + optional sign extension (`'s` flag).
    auto szv = as_int(dict_get(d, "size"));
    if (szv && szv->data() > 0 && szv->data() < 64) {
        auto size = static_cast<std::uint64_t>(szv->data());
        raw &= (1ull << size) - 1;
        bool is_signed = dict_get(d, "signed") != nullptr;
        if (is_signed && (raw & (1ull << (size - 1)))) {
            return static_cast<std::int64_t>(raw) -
                   static_cast<std::int64_t>(1ull << size);
        }
    }
    return static_cast<std::int64_t>(raw);
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

// `defined(X)` — pull X's identifier from the argument. Two AST shapes:
//   PP/synth:  args:[{type:"ref", value:"X"}]
//   SV COND_EXPR: args:[{style:"positional", value:{type:"ref", name:"X"}}]
std::optional<bool>
eval_defined(const std::shared_ptr<ArrayValue>& args, const RefResolver& resolver) {
    if (!args || args->data().empty()) return std::nullopt;
    auto arg0 = as_dict(args->data()[0]);
    if (!arg0) return std::nullopt;
    std::optional<std::string> name = dict_get_str(*arg0, "value");  // old shape
    if (!name) {
        // SV: unwrap the positional arg, then read the ref's `name`.
        auto inner = as_dict(dict_get(*arg0, "value"));
        const auto& ref = inner ? *inner : *arg0;
        name = dict_get_str(ref, "name");
        if (!name) name = dict_get_str(ref, "value");
    }
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

        // Ternary `?:` carries cond/then/else fields, not `args`.
        if (op == "?:") {
            auto c = eval_bool(dict_get(*d, "cond"), resolver);
            if (!c) return std::nullopt;
            return eval_int(dict_get(*d, *c ? "then" : "else"), resolver);
        }

        auto args = as_array(dict_get(*d, "args"));
        if (!args || args->data().empty()) return std::nullopt;

        // Unary `!` flips truthiness — surface as int 0/1.
        if (op == "!") {
            auto b = eval_bool(args->data()[0], resolver);
            if (!b) return std::nullopt;
            return *b ? 0 : 1;
        }

        // Unary arithmetic / bitwise-not (single operand). `-`/`+`
        // also appear as binary ops (≥2 args, handled below); the
        // arg count disambiguates. Bitwise reduction ops (`&`/`|`/`^`
        // with one operand) need bit-width we don't have — left to the
        // binary handlers, which reject the 1-arg case as undecidable.
        if (args->data().size() == 1 && (op == "-" || op == "+" || op == "~")) {
            auto a = eval_int(args->data()[0], resolver);
            if (!a) return std::nullopt;
            if (op == "-") return -*a;
            if (op == "~") return ~*a;
            return *a;  // unary +
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

        // Shifts: fold left-to-right. `<<`/`<<<` are the same for a
        // value (no width to overflow into); `>>` is logical (unsigned),
        // `>>>` arithmetic (sign-preserving). A shift ≥ 64 or negative
        // saturates to 0 (or all-ones for arithmetic-right of a negative).
        if (op == "<<" || op == ">>" || op == "<<<" || op == ">>>") {
            if (args->data().size() < 2) return std::nullopt;
            auto first = eval_int(args->data()[0], resolver);
            if (!first) return std::nullopt;
            std::int64_t acc = *first;
            for (std::size_t i = 1; i < args->data().size(); ++i) {
                auto n = eval_int(args->data()[i], resolver);
                if (!n) return std::nullopt;
                if (*n < 0 || *n >= 64) {
                    acc = (op == ">>>" && acc < 0) ? -1 : 0;
                    continue;
                }
                auto sh = static_cast<unsigned>(*n);
                if (op == "<<" || op == "<<<")
                    acc = static_cast<std::int64_t>(static_cast<std::uint64_t>(acc) << sh);
                else if (op == ">>")
                    acc = static_cast<std::int64_t>(static_cast<std::uint64_t>(acc) >> sh);
                else
                    acc >>= sh;  // >>> arithmetic (acc is signed)
            }
            return acc;
        }

        // Bitwise binary. One operand = a reduction op (needs bit-width) —
        // undecidable here. `^~`/`~^` are XNOR (bit-complement of XOR).
        if (op == "&" || op == "|" || op == "^" || op == "^~" || op == "~^") {
            if (args->data().size() < 2) return std::nullopt;
            auto first = eval_int(args->data()[0], resolver);
            if (!first) return std::nullopt;
            std::int64_t acc = *first;
            for (std::size_t i = 1; i < args->data().size(); ++i) {
                auto n = eval_int(args->data()[i], resolver);
                if (!n) return std::nullopt;
                if (op == "&")      acc &= *n;
                else if (op == "|") acc |= *n;
                else                acc ^= *n;  // ^, ^~, ~^
            }
            if (op == "^~" || op == "~^") acc = ~acc;
            return acc;
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
        if (type == "int" || type == "integer") {
            auto iv = as_int(dict_get(*d, "value"));
            if (!iv) return std::nullopt;
            return iv->data();
        }
        if (type == "paren") {
            return eval_int(dict_get(*d, "value"), resolver);
        }
        if (type == "based_num") {
            return eval_based_num(*d);
        }
        if (type == "default_value_pattern") {
            // `'0` → 0, `'1` → all-ones (-1). `'x`/`'z` are indeterminate.
            auto val = dict_get_str(*d, "value");
            if (!val) return std::nullopt;
            if (*val == "0") return 0;
            if (*val == "1") return -1;
            return std::nullopt;
        }
        if (type == "ref") {
            auto name = dict_get_str(*d, "value");
            if (!name) name = dict_get_str(*d, "name");  // SV ref uses `name`
            if (!name) return std::nullopt;
            return resolve_ref_as_int(*name, resolver);
        }
        if (type == "call" || type == "func_call") {
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

        // Ternary carries cond/then/else, not `args` — delegate to the
        // int evaluator (which selects a branch) and take truthiness.
        // Must precede the `args` guard below, which would otherwise
        // reject the argless ternary node as undecidable.
        if (op == "?:") {
            auto n = eval_int(v, resolver);
            if (!n) return std::nullopt;
            return *n != 0;
        }

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
        if (type == "int" || type == "integer") {
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
            if (!name) name = dict_get_str(*d, "name");  // SV ref uses `name`
            if (!name) return std::nullopt;
            if (!resolver) return std::nullopt;
            auto body = resolver(*name);
            if (!body) return false;
            if (auto n = parse_int(*body)) return *n != 0;
            return true;   // defined, body not int → truthy
        }
        if (type == "call" || type == "func_call") {
            auto name = dict_get_str(*d, "name");
            if (!name) return std::nullopt;
            if (*name == "defined") {
                return eval_defined(as_array(dict_get(*d, "args")), resolver);
            }
            return std::nullopt;
        }
    }
    // Leaf types with no dedicated bool rule (based_num,
    // default_value_pattern, …): evaluate as int, then take truthiness.
    if (auto n = eval_int(v, resolver)) return *n != 0;
    return std::nullopt;
}

} // namespace

ValuePtr default_pp_expr_eval(
    const ValuePtr& cond,
    const std::function<std::optional<std::string>(const std::string&)>& ref_resolver) {
    // Internal evaluation stays a tri-state optional<bool>; lift it to the
    // public ValuePtr tri-state at this boundary (nullopt -> Undefined).
    auto b = eval_bool(cond, ref_resolver);
    if (!b) return undefined_value();
    return *b ? true_value() : false_value();
}

ValuePtr Preprocessor::eval_cond_default(const ValuePtr& cond) {
    auto resolver =
        [this](const std::string& name) -> std::optional<std::string> {
            auto m = get_macro(name);
            if (!m) return std::nullopt;
            // Resolve the identifier to its macro's body expanded through the
            // ONE macro-expansion mechanism (render_macro_body_segments), NOT
            // a shallow body_text read. So a `\`if` condition sees nested and
            // backtick macros folded exactly as body text does — e.g.
            // `\`define W `\`X` / `\`define X 16` makes `\`if W > 8` see 16,
            // instead of the raw "`\`X" that body_text returned (undecidable).
            return render_macro_body_segments(*segment_body(m->body_raw));
        };
    // Already-structured cond (synthesized via process_ast) — evaluate it
    // directly.
    if (!as_string(cond)) return default_pp_expr_eval(cond, resolver);
    // Raw-text cond from the IF rule: parse it through PP_COND — a
    // transparent one-alternative wrapper over the SAME COND_EXPR the SV
    // expression ladder uses (no separate condition grammar). So the full
    // expression surface — arithmetic, comparisons, boolean ops, bitwise,
    // shifts, ternary, sized/based literals — parses here exactly as it
    // does anywhere else; default_pp_expr_eval folds all of it. A cond the
    // evaluator can't reduce to a constant (an undefined macro, an x/z
    // literal, a call it doesn't model) yields Undefined → on_undecidable,
    // rather than failing the whole preprocess.
    // PP_COND (not COND_EXPR directly) only exists to re-declare the
    // `ignore whitespace line_comment block_comment` policy that COND_EXPR's
    // children need but don't inherit through a bare parse_from entry — so
    // spaced conditions like `defined(A) && defined(B)` parse.
    // Expand the raw condition through the SAME mechanism FIRST (expand, then
    // evaluate — C-preprocessor order), so a backtick macro USE in the
    // condition (`\`if `\`W > 8`) folds before it's parsed. Bare identifiers
    // (`\`if FOO`) survive expansion and are resolved during the fold by the
    // `resolver` above — so both spellings, and nesting, go through Mechanism A.
    std::string expanded =
        render_macro_body_segments(*segment_body(as_string(cond)->data()));
    auto stream = Stream::from_string(expanded);
    auto r = pp_grammar_.parse_from(stream, "PP_COND");
    if (!r) return undefined_value();
    return default_pp_expr_eval(*r, resolver);
}

} // namespace rawast
