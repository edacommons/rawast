#include <rawast/preprocessor.hpp>

#include <rawast/grammar.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <cstdio>
#include <filesystem>
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

namespace {

// Scan forward past the end of one logical preprocessor directive
// line, returning the byte offset just past the terminating newline
// (or end-of-source). Handles `\\` line continuation in the same way
// sv_line_text does — a backslash immediately before a newline
// extends the directive to the next line.
std::size_t scan_past_directive_line(const std::string& source,
                                     std::size_t cursor) {
    while (cursor < source.size()) {
        char c = source[cursor];
        if (c == '\\' && cursor + 1 < source.size()
            && source[cursor + 1] == '\n') {
            cursor += 2;
            continue;
        }
        if (c == '\n') return cursor + 1;
        if (c == '\r') {
            if (cursor + 1 < source.size() && source[cursor + 1] == '\n') {
                return cursor + 2;
            }
            return cursor + 1;
        }
        ++cursor;
    }
    return cursor;
}

} // namespace

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
    // Reset the source map for this call. Spans are scoped to one
    // process(); callers that need to retain a map across calls
    // (multi-file workflows) snapshot the state themselves.
    state_.spans.clear();
    // Root span: covers the entire input text. No parent. Source-
    // structure only (no output offset of its own — child spans
    // record output ranges).
    std::uint32_t root_id =
        record_span(Span::NoParent, /*parent_offset=*/0,
                    text.size(), Span::NoOutput,
                    state_.current_file.empty() ? "<input>"
                                                : state_.current_file);
    std::string out;
    std::size_t src_cursor = 0;
    walk(*parsed, out, src_cursor, text, root_id);
    return out;
}

std::uint32_t Preprocessor::record_span(std::uint32_t parent_id,
                                       std::size_t parent_offset,
                                       std::size_t length,
                                       std::size_t out_offset,
                                       std::string name) {
    Span s;
    s.id = static_cast<std::uint32_t>(state_.spans.size());
    s.parent_id = parent_id;
    s.parent_offset = parent_offset;
    s.length = length;
    s.out_offset = out_offset;
    s.name = std::move(name);
    state_.spans.push_back(s);
    return s.id;
}

std::vector<SourceFrame>
Preprocessor::stack_at(std::size_t out_offset) const {
    // Find the span whose [out_offset, out_offset+length) range
    // contains the queried offset. Output spans are the ones with
    // out_offset != NoOutput; iterate (size is small in practice,
    // a sorted vector + binary search is a polish-phase improvement).
    const Span* leaf = nullptr;
    std::size_t leaf_in = 0;
    for (const auto& s : state_.spans) {
        if (s.out_offset == Span::NoOutput) continue;
        if (out_offset >= s.out_offset
            && out_offset < s.out_offset + s.length) {
            leaf = &s;
            leaf_in = out_offset - s.out_offset;
            break;
        }
    }
    if (!leaf) return {};

    std::vector<SourceFrame> frames;
    const Span* cur = leaf;
    std::size_t cur_offset = leaf_in;
    while (cur) {
        frames.push_back({cur->name, cur_offset});
        if (cur->parent_id == Span::NoParent) break;
        if (cur->parent_id >= state_.spans.size()) break;
        // The byte at `cur_offset` within `cur` maps to
        // `cur->parent_offset + cur_offset` within the parent.
        cur_offset = cur->parent_offset + cur_offset;
        cur = &state_.spans[cur->parent_id];
    }
    return frames;
}

namespace {

// Position the cursor at the start of the item's source span. The
// preprocessor grammar's `ignore linespace` skips horizontal
// whitespace silently — the AST doesn't carry those skipped bytes,
// so the walker can't blindly advance src_cursor by the captured
// text's length. Instead, for each item we search for a characteristic
// prefix (the literal opening token for a directive, or the captured
// text itself for PP_TEXT) starting at the current cursor; the match
// position is the item's true source offset.
//
// Returns the byte offset where the item begins, or src_cursor
// unchanged if no match is found (defensive fallback — the map
// degrades to approximate, but no out-of-bounds writes).
std::size_t locate_item(const std::string& source,
                        std::size_t src_cursor,
                        const std::string& prefix) {
    if (prefix.empty()) return src_cursor;
    auto found = source.find(prefix, src_cursor);
    return found == std::string::npos ? src_cursor : found;
}

} // namespace

void Preprocessor::walk(const ValuePtr& v, std::string& out,
                        std::size_t& src_cursor,
                        const std::string& source,
                        std::uint32_t parent_span_id) {
    if (!v) return;

    if (auto arr = std::dynamic_pointer_cast<ArrayValue>(v)) {
        for (const auto& child : arr->data()) {
            // Skip pure-whitespace strings: these are emissions from
            // terminals like `sv_eol` matched inside a sequence-array
            // sub-rule (e.g. PP_ELSE_CLAUSE's `\`else, sv_eol, repeat
            // <PP_ITEM>`). They've already been consumed at parse
            // time; walking them would advance the cursor forward to
            // the next matching whitespace in source and skip real
            // content.
            if (auto s = std::dynamic_pointer_cast<StringValue>(child)) {
                bool only_ws = true;
                for (char c : s->data()) {
                    if (c != ' ' && c != '\t'
                        && c != '\n' && c != '\r') {
                        only_ws = false;
                        break;
                    }
                }
                if (only_ws) continue;
            }
            walk(child, out, src_cursor, source, parent_span_id);
        }
        return;
    }

    if (auto dict = std::dynamic_pointer_cast<DictValue>(v)) {
        auto type = dict_string_or_empty(*dict, "type");

        if (type == "text") {
            auto text = dict_string_or_empty(*dict, "text");
            if (!text.empty()) {
                // Find the captured text in source. `origin` is where
                // sv_pp_text_line started consuming — usually AFTER any
                // whitespace the grammar's `ignore linespace` policy
                // already ate. To preserve indentation in the output,
                // back up to the start of the current line (previous
                // `\n` + 1, or 0 if we're on the first line) and emit
                // everything from there through origin + text length.
                std::size_t origin = locate_item(source, src_cursor, text);
                std::size_t line_start = origin;
                while (line_start > src_cursor && line_start > 0
                       && source[line_start - 1] != '\n'
                       && source[line_start - 1] != '\r') {
                    --line_start;
                }
                std::size_t end = origin + text.size();
                std::size_t emit_len = end - line_start;
                record_span(parent_span_id, line_start,
                            emit_len, out.size(), "text");
                out.append(source, line_start, emit_len);
                src_cursor = end;
            }
            return;
        }
        if (type == "define") {
            // Position the cursor at the start of the directive
            // before consuming the line — leading whitespace
            // that ignore-policy ate isn't part of the AST.
            src_cursor = locate_item(source, src_cursor, "`define");
            handle_define(*dict);
            src_cursor = scan_past_directive_line(source, src_cursor);
            return;
        }
        if (type == "undef") {
            src_cursor = locate_item(source, src_cursor, "`undef");
            handle_undef(*dict);
            src_cursor = scan_past_directive_line(source, src_cursor);
            return;
        }
        if (type == "ifdef") {
            handle_ifdef(*dict, out, src_cursor, source,
                         parent_span_id, /*invert=*/false);
            return;
        }
        if (type == "ifndef") {
            handle_ifdef(*dict, out, src_cursor, source,
                         parent_span_id, /*invert=*/true);
            return;
        }
        if (type == "macro_use") {
            handle_macro_use(*dict, out, src_cursor, source,
                             parent_span_id);
            return;
        }
        if (type == "include") {
            handle_include(*dict, out, src_cursor, source,
                           parent_span_id);
            return;
        }
        // Unknown type — recurse into the dict's values so wrapper
        // dicts (no role of their own) still propagate their contents.
        for (const auto& [_, child] : dict->data()) {
            walk(child, out, src_cursor, source, parent_span_id);
        }
        return;
    }

    if (auto sv = std::dynamic_pointer_cast<StringValue>(v)) {
        auto& s = sv->data();
        if (!s.empty()) {
            std::size_t origin = locate_item(source, src_cursor, s);
            record_span(parent_span_id, origin,
                        s.size(), out.size(), "text");
            out += s;
            src_cursor = origin + s.size();
        }
        return;
    }
    // Other value kinds (null/int/float/bool) don't appear in a
    // well-formed preprocessor AST; ignore rather than fail.
}

namespace {

void trim_horiz(std::string& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.erase(0, 1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
}

// Split a parameter list (the text BETWEEN `(` and `)`, exclusive)
// into trimmed identifier names. Splits on `,` at paren depth 0
// so a param like `f(x, y)` would stay one token — though real
// macro parameter lists are flat identifiers in practice.
std::vector<std::string> split_params(const std::string& body) {
    std::vector<std::string> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (c == ',' && depth == 0) {
            std::string p = body.substr(start, i - start);
            trim_horiz(p);
            if (!p.empty()) out.push_back(std::move(p));
            start = i + 1;
        }
    }
    std::string last = body.substr(start);
    trim_horiz(last);
    if (!last.empty()) out.push_back(std::move(last));
    return out;
}

// Substitute parameter identifiers in `body` with the corresponding
// arg values, also handling SV preprocessor token operators:
//
//   `\`\``       — token paste: remove the operator; adjacent
//                  tokens fuse because there's no whitespace
//                  between them.
//   `\`"…\`"`    — stringification: replace each `\`"` with a
//                  literal `"`. Param substitution inside the
//                  delimiters happens via the same scan; the
//                  enclosed text becomes a string literal at
//                  expansion time.
//
// Phase 2.5 scope: structural support. Escape edge cases inside
// `\`"…\`"` (a stray `"` in an arg) are left to host parsing —
// the SV LRM is permissive here too.
std::string substitute_params(const std::string& body,
                              const std::vector<std::string>& params,
                              const std::vector<std::string>& args) {
    std::unordered_map<std::string, std::string> map;
    bool has_params = (params.size() == args.size()) && !params.empty();
    if (has_params) {
        for (std::size_t i = 0; i < params.size(); ++i) {
            std::string arg = args[i];
            trim_horiz(arg);
            map[params[i]] = std::move(arg);
        }
    }
    std::string out;
    out.reserve(body.size());
    std::size_t i = 0;
    while (i < body.size()) {
        // `\`"` — stringification marker (open or close). Emit a
        // literal `"`; the contents between the open and close
        // markers are subject to the same param substitution loop.
        if (i + 1 < body.size() && body[i] == '`' && body[i + 1] == '"') {
            out.push_back('"');
            i += 2;
            continue;
        }
        // `\`\`` — token paste. Drop the two backticks; adjacent
        // tokens naturally concatenate because no whitespace
        // separates them.
        if (i + 1 < body.size() && body[i] == '`' && body[i + 1] == '`') {
            i += 2;
            continue;
        }
        unsigned char uc = static_cast<unsigned char>(body[i]);
        if (std::isalpha(uc) || body[i] == '_') {
            std::size_t start = i;
            while (i < body.size()) {
                unsigned char c = static_cast<unsigned char>(body[i]);
                if (!std::isalnum(c) && body[i] != '_' && body[i] != '$') {
                    break;
                }
                ++i;
            }
            std::string ident = body.substr(start, i - start);
            auto it = map.find(ident);
            if (has_params && it != map.end()) out += it->second;
            else out += ident;
        } else {
            out.push_back(body[i]);
            ++i;
        }
    }
    return out;
}

// Scan from `cursor` over an identifier-shaped run (start char
// alpha/underscore, continuation alpha/digit/underscore/$).
// Returns the offset just past the identifier; equals `cursor` if
// nothing identifier-shaped is present.
std::size_t scan_identifier(const std::string& text, std::size_t cursor) {
    if (cursor >= text.size()) return cursor;
    unsigned char first = static_cast<unsigned char>(text[cursor]);
    if (!std::isalpha(first) && text[cursor] != '_') return cursor;
    ++cursor;
    while (cursor < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[cursor]);
        if (!std::isalnum(c) && text[cursor] != '_' && text[cursor] != '$') {
            break;
        }
        ++cursor;
    }
    return cursor;
}

// Scan a parenthesised, depth-balanced argument list starting at
// `cursor` (which must point at `(`). Splits args at depth-0
// commas. Returns the offset just past the closing `)` and
// populates `args`. If the list is unbalanced, returns `cursor`
// unchanged and leaves `args` empty (caller treats as no-args).
std::size_t scan_args(const std::string& text, std::size_t cursor,
                      std::vector<std::string>& args) {
    if (cursor >= text.size() || text[cursor] != '(') return cursor;
    int depth = 1;
    std::size_t k = cursor + 1;
    std::size_t arg_start = k;
    while (k < text.size() && depth > 0) {
        char c = text[k];
        if (c == '(') ++depth;
        else if (c == ')') {
            --depth;
            if (depth == 0) {
                args.push_back(text.substr(arg_start, k - arg_start));
                ++k;
                return k;
            }
        } else if (c == ',' && depth == 1) {
            args.push_back(text.substr(arg_start, k - arg_start));
            arg_start = k + 1;
        }
        ++k;
    }
    // Unbalanced — restore the not-found state.
    args.clear();
    return cursor;
}

} // namespace

std::string Preprocessor::expand_recursive(const std::string& text) {
    if (state_.current_depth >= opts_.max_expansion_depth) {
        state_.warnings.push_back(
            {"preprocessor: max_expansion_depth (" +
             std::to_string(opts_.max_expansion_depth) +
             ") reached; emitting verbatim",
             state_.current_file, state_.current_line});
        return text;
    }

    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c != '`') {
            out.push_back(c);
            ++i;
            continue;
        }
        // Found `\``. Scan the identifier that follows.
        std::size_t name_start = i + 1;
        std::size_t name_end = scan_identifier(text, name_start);
        if (name_end == name_start) {
            // Bare backtick — not a macro use. Emit verbatim.
            out.push_back(c);
            ++i;
            continue;
        }
        std::string name = text.substr(name_start, name_end - name_start);

        // Blue-paint guard — break cycles by leaving the call site
        // verbatim if the same name is already being expanded.
        if (state_.active_expansions.count(name)) {
            out.append(text, i, name_end - i);
            i = name_end;
            continue;
        }

        auto it = state_.macros.find(name);
        if (it == state_.macros.end()) {
            // Undefined in nested context — leave verbatim. The
            // top-level `on_undefined` policy applies at the
            // PP_MACRO_USE walker entry; nested undefined uses
            // pass through so the host parser still sees them.
            out.append(text, i, name_end - i);
            i = name_end;
            continue;
        }

        const auto& macro = it->second;
        std::vector<std::string> args;
        std::size_t after_args = name_end;
        if (macro.is_function_like && name_end < text.size()
            && text[name_end] == '(') {
            after_args = scan_args(text, name_end, args);
            if (after_args == name_end) {
                // Unbalanced parens — give up on this expansion.
                out.append(text, i, name_end - i);
                i = name_end;
                continue;
            }
        }

        std::string body = macro.body;
        if (macro.is_function_like) {
            if (macro.params.size() == args.size()) {
                body = substitute_params(body, macro.params, args);
            } else {
                state_.warnings.push_back(
                    {"macro `" + name + " expects " +
                     std::to_string(macro.params.size()) +
                     " args, got " + std::to_string(args.size()),
                     state_.current_file, state_.current_line});
                // emit substituted-anyway body verbatim — caller
                // can decide whether this is acceptable.
            }
        }

        // Recurse with blue paint on this name.
        state_.active_expansions.insert(name);
        ++state_.current_depth;
        std::string expanded = expand_recursive(body);
        --state_.current_depth;
        state_.active_expansions.erase(name);

        out += expanded;
        i = after_args;
    }
    return out;
}

void Preprocessor::handle_define(const DictValue& d) {
    MacroDef m;
    m.name = dict_string_or_empty(d, "name");
    if (m.name.empty()) return;
    auto raw_body = dict_string_or_empty(d, "body");

    // Detect function-like form: body begins with `(`. The SV LRM
    // strict distinction (no space between name and `(`) is lost
    // because the grammar's `ignore linespace` eats any spaces
    // there — pragmatically fine since UVM / Ibex code always
    // writes function-like macros with no space anyway.
    //
    // Find the matching `)` at paren depth 0; if it's followed by
    // body content, the prefix is the parameter list. If matching
    // fails or `(` is unmatched, fall back to object-like.
    if (!raw_body.empty() && raw_body.front() == '(') {
        int depth = 1;
        std::size_t i = 1;
        while (i < raw_body.size() && depth > 0) {
            char c = raw_body[i];
            if (c == '(') ++depth;
            else if (c == ')') --depth;
            if (depth > 0) ++i;
        }
        if (depth == 0) {
            // raw_body[1..i] = params (exclusive of parens),
            // raw_body[i+1..] = body content (with leading space).
            m.params = split_params(raw_body.substr(1, i - 1));
            std::string body = raw_body.substr(i + 1);
            trim_horiz(body);
            m.body = std::move(body);
            m.is_function_like = true;
            state_.macros[m.name] = std::move(m);
            return;
        }
    }
    // Object-like: body is the captured text verbatim.
    m.body = std::move(raw_body);
    m.is_function_like = false;
    state_.macros[m.name] = std::move(m);
}

void Preprocessor::handle_undef(const DictValue& d) {
    auto name = dict_string_or_empty(d, "name");
    if (!name.empty()) state_.macros.erase(name);
}

void Preprocessor::handle_macro_use(const DictValue& d, std::string& out,
                                    std::size_t& src_cursor,
                                    const std::string& source,
                                    std::uint32_t parent_span_id) {
    auto name = dict_string_or_empty(d, "name");
    if (name.empty()) return;

    // Pull raw argument strings out of the AST. PP_MACRO_ARGS
    // captures them as an array of sv_balanced_arg strings —
    // each arg is the raw text between commas at paren depth 0.
    std::vector<std::string> args;
    if (auto args_val = dict_value_or_null(d, "args")) {
        if (auto arr = std::dynamic_pointer_cast<ArrayValue>(args_val)) {
            for (const auto& a : arr->data()) {
                if (auto s = std::dynamic_pointer_cast<StringValue>(a)) {
                    args.push_back(s->data());
                }
            }
        }
    }

    // Locate the `\`NAME` site in the source. PP_MACRO_USE consumes
    // a full line (including the trailing newline via sv_eol), so
    // the source span spans from the backtick to end-of-line.
    std::size_t use_src_start = locate_item(source, src_cursor, "`" + name);
    std::size_t use_src_end = scan_past_directive_line(source, use_src_start);
    std::size_t use_src_len = use_src_end - use_src_start;
    bool consumed_newline = use_src_end > use_src_start
        && (source[use_src_end - 1] == '\n' || source[use_src_end - 1] == '\r');

    auto emit_expansion = [&](const std::string& body, const std::string& span_name) {
        // PP_MACRO_USE in our line-based grammar consumes the
        // trailing newline via sv_eol. Replicate it on the
        // expansion side so the output keeps line structure.
        record_span(parent_span_id, use_src_start, use_src_len,
                    out.size(), span_name);
        out += body;
        if (consumed_newline
            && (body.empty() || body.back() != '\n')) {
            out += '\n';
        }
    };

    auto it = state_.macros.find(name);
    if (it != state_.macros.end()) {
        const auto& macro = it->second;
        std::string substituted;
        if (macro.is_function_like && macro.params.size() != args.size()) {
            // Arity mismatch — warn; still run substitute_params to
            // process \`"…\`" stringification and \`\` token paste,
            // which should fire regardless of param substitution.
            state_.warnings.push_back(
                {"macro `" + name + " expects " +
                 std::to_string(macro.params.size()) +
                 " args, got " + std::to_string(args.size()),
                 state_.current_file, state_.current_line});
            substituted = substitute_params(macro.body, {}, {});
        } else {
            // Function-like with matching arity OR object-like
            // (no params). The substitution helper handles both
            // cases — empty params means identifier passes through
            // but token operators still fire.
            substituted = substitute_params(macro.body, macro.params, args);
        }
        // Recursively expand inline macro uses inside the body.
        // The current macro's own name is added to active_expansions
        // before recursing so `\`A`-in-`\`A`-body becomes a verbatim
        // emit instead of infinite recursion.
        state_.active_expansions.insert(name);
        ++state_.current_depth;
        std::string emitted = expand_recursive(substituted);
        --state_.current_depth;
        state_.active_expansions.erase(name);
        emit_expansion(emitted, "macro " + name + " expansion");
        src_cursor = use_src_end;
        return;
    }

    // Undefined — apply policy.
    auto leave_emit = [&]() {
        std::string emit = "`" + name;
        if (!args.empty()) {
            emit += "(";
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i > 0) emit += ",";
                emit += args[i];
            }
            emit += ")";
        }
        emit_expansion(emit, "undefined macro use");
    };

    switch (opts_.on_undefined) {
        case PpOnUndefined::Leave:
            leave_emit();
            src_cursor = use_src_end;
            return;
        case PpOnUndefined::Empty:
            src_cursor = use_src_end;
            return;
        case PpOnUndefined::Warn:
            state_.warnings.push_back(
                {"undefined macro `" + name, state_.current_file,
                 state_.current_line});
            leave_emit();
            src_cursor = use_src_end;
            return;
        case PpOnUndefined::Error:
            throw std::runtime_error("undefined macro `" + name);
    }
}

void Preprocessor::handle_include(const DictValue& d, std::string& out,
                                  std::size_t& src_cursor,
                                  const std::string& source,
                                  std::uint32_t parent_span_id) {
    auto path = dict_string_or_empty(d, "path");
    // Advance the source cursor past the `\`include "path"\n` line —
    // whether we successfully process the included file or not.
    src_cursor = locate_item(source, src_cursor, "`include");
    std::size_t directive_end = scan_past_directive_line(source, src_cursor);
    src_cursor = directive_end;

    if (path.empty()) {
        state_.warnings.push_back(
            {"`include: empty path", state_.current_file, state_.current_line});
        return;
    }

    // Resolve path. Try each include_paths entry in order, then the
    // current_file's directory (so relative `\`include "foo.svh"`
    // works when foo.svh is alongside the including file), then the
    // path as given (covers absolute paths and CWD-relative).
    namespace fs = std::filesystem;
    fs::path resolved;
    bool found = false;
    fs::path requested = path;
    auto try_candidate = [&](const fs::path& base) {
        fs::path candidate = base.empty() ? requested : (base / requested);
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec) {
            resolved = candidate;
            found = true;
            return true;
        }
        return false;
    };
    for (const auto& dir : opts_.include_paths) {
        if (try_candidate(dir)) break;
    }
    if (!found && !state_.current_file.empty()) {
        fs::path parent = fs::path(state_.current_file).parent_path();
        try_candidate(parent);
    }
    if (!found) try_candidate(fs::path{});
    if (!found) {
        state_.warnings.push_back(
            {"`include: file not found: '" + path + "'",
             state_.current_file, state_.current_line});
        return;
    }

    std::string canonical = resolved.lexically_normal().string();
    bool already_seen = false;
    for (const auto& p : state_.included_files) {
        if (p == canonical) { already_seen = true; break; }
    }
    if (!already_seen) state_.included_files.push_back(canonical);

    std::ifstream f(resolved);
    if (!f.is_open()) {
        state_.warnings.push_back(
            {"`include: failed to open '" + canonical + "'",
             state_.current_file, state_.current_line});
        return;
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    std::string include_text = buf.str();

    // Parse the included file with the same preprocessor grammar.
    // (No depth-of-includes check yet — Phase 3 polish.)
    std::istringstream is(include_text);
    StreamReader sr{is};
    auto parsed = pp_grammar_.parse(sr);
    if (!parsed) {
        state_.warnings.push_back(
            {"`include: parse failed in '" + canonical + "': " +
             parsed.error().message,
             state_.current_file, state_.current_line});
        return;
    }

    // New file context — push/restore so nested directives report
    // the correct current_file in any warnings they emit.
    auto saved_file = state_.current_file;
    auto saved_line = state_.current_line;
    state_.current_file = canonical;
    state_.current_line = 1;

    // Create a Span describing the included file. The include site
    // (use_src_start..directive_end in the outer source) is the parent
    // for hierarchical lookup.
    std::size_t use_src_start = locate_item(source, /*from=*/src_cursor - 1 - path.size(),
                                            "`include");
    if (use_src_start > directive_end) use_src_start = directive_end;
    std::uint32_t include_span =
        record_span(parent_span_id, use_src_start,
                    include_text.size(),
                    /*out_offset=*/Span::NoOutput, canonical);

    // Walk the included file. In splice mode, output bytes go into
    // the caller's `out`; in side-effects mode, they go into a
    // discard buffer (state still mutates — that's the side effect).
    std::size_t include_src_cursor = 0;
    if (opts_.splice) {
        walk(*parsed, out, include_src_cursor, include_text, include_span);
    } else {
        std::string discard;
        std::size_t saved_spans = state_.spans.size();
        walk(*parsed, discard, include_src_cursor, include_text, include_span);
        // Drop spans that mapped into the discarded output —
        // they're not reachable from anything in `out`.
        state_.spans.resize(saved_spans);
    }

    state_.current_file = std::move(saved_file);
    state_.current_line = saved_line;
}

void Preprocessor::handle_ifdef(const DictValue& d, std::string& out,
                                std::size_t& src_cursor,
                                const std::string& source,
                                std::uint32_t parent_span_id,
                                bool invert) {
    auto cond = dict_string_or_empty(d, "cond");
    bool defined = !cond.empty() &&
                   state_.macros.find(cond) != state_.macros.end();
    bool take_body = invert ? !defined : defined;

    // Position cursor at the `\`ifdef`/`\`ifndef` keyword, then advance
    // past the directive line. Same locate_item logic handles
    // leading whitespace the grammar's ignore policy ate.
    src_cursor = locate_item(source, src_cursor,
                             invert ? "`ifndef" : "`ifdef");
    src_cursor = scan_past_directive_line(source, src_cursor);

    auto walk_or_discard = [&](const ValuePtr& branch, bool emit) {
        if (!branch) return;
        if (emit) {
            walk(branch, out, src_cursor, source, parent_span_id);
        } else {
            std::string discard;
            std::size_t saved = state_.spans.size();
            walk(branch, discard, src_cursor, source, parent_span_id);
            // Discarded branch's spans aren't in the output stream.
            state_.spans.resize(saved);
        }
    };

    walk_or_discard(dict_value_or_null(d, "body"), take_body);

    if (auto eb = dict_value_or_null(d, "else_branch")) {
        // Position past the `\`else` line.
        src_cursor = locate_item(source, src_cursor, "`else");
        src_cursor = scan_past_directive_line(source, src_cursor);
        walk_or_discard(eb, !take_body);
    }

    // Position past `\`endif`.
    src_cursor = locate_item(source, src_cursor, "`endif");
    src_cursor = scan_past_directive_line(source, src_cursor);
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
