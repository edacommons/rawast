#include <rawast/preprocessor.hpp>

#include <rawast/grammar.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace rawast {

namespace {

// Helpers for reading conventional fields from a parsed dict. The
// walker uses these to pull `type`, `name`, `body`, `cond`, etc. from
// the value tree the preprocessor grammar produces. Missing or wrong-
// typed fields return empty rather than throwing — Phase 1.2 keeps
// the policy permissive; tightening (with a grammar-validation step)
// is Phase 3 polish.

std::string dict_string_or_empty(const DictValue& d,
                                 const std::string& key) {
    auto it = d.data().find(key);
    if (it == d.data().end() || !it->second) return {};
    auto sv = std::dynamic_pointer_cast<StringValue>(it->second);
    return sv ? sv->data() : std::string{};
}

ValuePtr dict_value_or_null(const DictValue& d, const std::string& key) {
    auto it = d.data().find(key);
    if (it == d.data().end()) return nullptr;
    return it->second;
}

} // namespace

std::string_view to_string(PpRole role) noexcept {
    switch (role) {
        case PpRole::None:      return "";
        case PpRole::Define:    return "define";
        case PpRole::Undef:     return "undef";
        case PpRole::Ifdef:     return "ifdef";
        case PpRole::Ifndef:    return "ifndef";
        case PpRole::If:        return "if";
        case PpRole::Elsif:     return "elsif";
        case PpRole::Else:      return "else";
        case PpRole::Endif:     return "endif";
        case PpRole::Include:   return "include";
        case PpRole::MacroUse:  return "macro_use";
        case PpRole::Paste:     return "paste";
        case PpRole::Stringify: return "stringify";
        case PpRole::Text:      return "text";
    }
    return "";
}

std::optional<PpRole> parse_pp_role(std::string_view name) noexcept {
    if (name == "define")    return PpRole::Define;
    if (name == "undef")     return PpRole::Undef;
    if (name == "ifdef")     return PpRole::Ifdef;
    if (name == "ifndef")    return PpRole::Ifndef;
    if (name == "if")        return PpRole::If;
    if (name == "elsif")     return PpRole::Elsif;
    if (name == "else")      return PpRole::Else;
    if (name == "endif")     return PpRole::Endif;
    if (name == "include")   return PpRole::Include;
    if (name == "macro_use") return PpRole::MacroUse;
    if (name == "paste")     return PpRole::Paste;
    if (name == "stringify") return PpRole::Stringify;
    if (name == "text")      return PpRole::Text;
    return std::nullopt;
}

std::string_view to_string(PpOnUndefined v) noexcept {
    switch (v) {
        case PpOnUndefined::Leave: return "leave";
        case PpOnUndefined::Error: return "error";
        case PpOnUndefined::Warn:  return "warn";
        case PpOnUndefined::Empty: return "empty";
    }
    return "";
}

std::optional<PpOnUndefined> parse_pp_on_undefined(std::string_view name) noexcept {
    if (name == "leave") return PpOnUndefined::Leave;
    if (name == "error") return PpOnUndefined::Error;
    if (name == "warn")  return PpOnUndefined::Warn;
    if (name == "empty") return PpOnUndefined::Empty;
    return std::nullopt;
}

// ─── Preprocessor ───────────────────────────────────────────────────

Preprocessor::Preprocessor(const Grammar& pp_grammar, PpOptions opts)
    : pp_grammar_(pp_grammar), opts_(std::move(opts)) {
    // If the user provided `predefined` content, process it now so the
    // resulting macro definitions (and any include side-effects) are
    // visible to the first real call to process / process_file. Done
    // here rather than lazily on the first user call so inspection
    // accessors (macros(), included_files()) reflect predefined
    // state immediately after construction.
    if (!opts_.predefined.empty()) {
        // The actual walker arrives in the next commit. For now,
        // record the predefined text as state we'd process; the
        // walker hookup will replace this stub.
        (void)process(opts_.predefined);
    }
}

std::string Preprocessor::process(const std::string& text) {
    // Guard against a grammar with no top rule set — the parse
    // engine derefs the top NodeId and would segfault. A no-rule
    // grammar is a degenerate but legitimate construction (e.g. in
    // skeleton tests before sv_preprocessor.rawast lands); pass
    // text through unchanged in that case.
    if (!pp_grammar_.top().valid()) return text;

    std::istringstream is(text);
    StreamReader sr{is};
    auto parsed = pp_grammar_.parse(sr);
    if (!parsed) {
        state_.warnings.push_back(
            {"preprocessor parse failed: " + parsed.error().message,
             state_.current_file, state_.current_line});
        // Pass-through on parse failure — better than silently
        // truncating the host's input. Host parser will get the
        // raw text and produce its own diagnostic if it can't
        // handle the directives.
        return text;
    }
    std::string out;
    walk(*parsed, out);
    return out;
}

void Preprocessor::walk(const ValuePtr& v, std::string& out) {
    if (!v) return;

    if (auto arr = std::dynamic_pointer_cast<ArrayValue>(v)) {
        for (const auto& child : arr->data()) walk(child, out);
        return;
    }

    if (auto dict = std::dynamic_pointer_cast<DictValue>(v)) {
        // Sentinel for the role of this directive. The preprocessor
        // grammar's role-bearing rules emit a `type` field via const
        // binding (`:type="define"` etc.). Plain content dicts that
        // happen to lack a `type` field fall through to the default
        // case — recurse into their values so wrapper rules (e.g.
        // PP_FILE: { commands: <PP_ITEMS> }) don't drop content.
        auto type = dict_string_or_empty(*dict, "type");

        if (type == "text") {
            out += dict_string_or_empty(*dict, "text");
            return;
        }
        if (type == "define") {
            handle_define(*dict);
            return;
        }
        if (type == "undef") {
            handle_undef(*dict);
            return;
        }
        if (type == "ifdef") {
            handle_ifdef(*dict, out, /*invert=*/false);
            return;
        }
        if (type == "ifndef") {
            handle_ifdef(*dict, out, /*invert=*/true);
            return;
        }
        if (type == "macro_use") {
            handle_macro_use(*dict, out);
            return;
        }
        // Unknown type — recurse into the dict's values so wrapper
        // dicts (no role of their own) still propagate their contents.
        for (const auto& [_, child] : dict->data()) walk(child, out);
        return;
    }

    if (auto sv = std::dynamic_pointer_cast<StringValue>(v)) {
        out += sv->data();
        return;
    }
    // Other value kinds (null/int/float/bool) don't appear in a
    // well-formed preprocessor AST; ignore rather than fail.
}

void Preprocessor::handle_define(const DictValue& d) {
    MacroDef m;
    m.name = dict_string_or_empty(d, "name");
    if (m.name.empty()) return;
    m.body = dict_string_or_empty(d, "body");
    m.is_function_like = false;  // Phase 1.2: object-like only.
    state_.macros[m.name] = std::move(m);
}

void Preprocessor::handle_undef(const DictValue& d) {
    auto name = dict_string_or_empty(d, "name");
    if (!name.empty()) state_.macros.erase(name);
}

void Preprocessor::handle_macro_use(const DictValue& d, std::string& out) {
    auto name = dict_string_or_empty(d, "name");
    if (name.empty()) return;
    auto it = state_.macros.find(name);
    if (it != state_.macros.end()) {
        // Object-like expansion: emit the body verbatim. Function-
        // like with arg substitution and recursive expansion arrive
        // in Phase 2.
        out += it->second.body;
        return;
    }
    // Undefined — apply policy.
    switch (opts_.on_undefined) {
        case PpOnUndefined::Leave:
            out += "`";
            out += name;
            return;
        case PpOnUndefined::Empty:
            return;
        case PpOnUndefined::Warn:
            state_.warnings.push_back(
                {"undefined macro `" + name, state_.current_file,
                 state_.current_line});
            out += "`";
            out += name;
            return;
        case PpOnUndefined::Error:
            throw std::runtime_error(
                "undefined macro `" + name);
    }
}

void Preprocessor::handle_ifdef(const DictValue& d, std::string& out,
                                bool invert) {
    auto cond = dict_string_or_empty(d, "cond");
    bool defined = !cond.empty() &&
                   state_.macros.find(cond) != state_.macros.end();
    bool take_body = invert ? !defined : defined;
    if (take_body) {
        walk(dict_value_or_null(d, "body"), out);
    } else if (auto eb = dict_value_or_null(d, "else_branch")) {
        walk(eb, out);
    }
}

std::string Preprocessor::process_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        state_.warnings.push_back(
            {"failed to open file '" + path + "'", path, 0});
        return {};
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    auto prev_file = std::move(state_.current_file);
    auto prev_line = state_.current_line;
    state_.current_file = path;
    state_.current_line = 1;
    // Record the file on first encounter (include-once behavior).
    bool already_seen = false;
    for (const auto& p : state_.included_files) {
        if (p == path) { already_seen = true; break; }
    }
    if (!already_seen) state_.included_files.push_back(path);
    auto out = process(buf.str());
    state_.current_file = std::move(prev_file);
    state_.current_line = prev_line;
    return out;
}

bool Preprocessor::is_defined(const std::string& name) const noexcept {
    return state_.macros.find(name) != state_.macros.end();
}

const MacroDef* Preprocessor::get_macro(const std::string& name) const noexcept {
    auto it = state_.macros.find(name);
    return it == state_.macros.end() ? nullptr : &it->second;
}

} // namespace rawast
