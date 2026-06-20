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

    auto stream = Stream::from_string(text);
    auto parsed = pp_grammar_.parse(stream);
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
    return process_ast(*parsed, text);
}

tl::expected<ValuePtr, ParseError>
Preprocessor::parse(const std::string& source) {
    if (!pp_grammar_.top().valid()) {
        return tl::unexpected(ParseError{
            {}, "Preprocessor::parse: pp_grammar has no top rule"});
    }
    auto stream = Stream::from_string(source);
    return pp_grammar_.parse(stream);
}

Stream Preprocessor::preprocess(const ValuePtr& ast,
                                 const std::string& source) {
    // Eager expansion into a heap-owned string; the Stream owns the
    // buffer via owner_, so the returned Stream is self-contained.
    auto expanded = std::make_shared<std::string>(process_ast(ast, source));
    auto is = std::make_unique<std::istringstream>(*expanded);
    return Stream(std::move(is), std::move(expanded));
}

std::string Preprocessor::process_ast(const ValuePtr& ast,
                                       const std::string& source) {
    // Reset the source map for this call. Spans are scoped to one
    // process_*() invocation; callers that need to retain a map
    // across calls (multi-file workflows) snapshot the state
    // themselves.
    state_.spans.clear();
    // Root span: covers the entire input text. No parent. Source-
    // structure only (no output offset of its own — child spans
    // record output ranges).
    std::uint32_t root_id =
        record_span(Span::NoParent, /*parent_offset=*/0,
                    source.size(), Span::NoOutput,
                    state_.current_file.empty() ? "<input>"
                                                : state_.current_file);
    std::string out;
    std::size_t src_cursor = 0;
    walk(ast, out, src_cursor, source, root_id);
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
        if (type == "text_line") {
            // sv_preprocessor.rawast TEXT_LINE produces
            //   { type:"text_line", segments:[ ...mixed... ] }
            // where each segment is either a bare StringValue (a
            // run of literal text) or a DictValue for an embedded
            // `\`NAME[(args)]` invocation. Iterate in order: text
            // runs are appended verbatim, macro_use dicts go
            // through handle_macro_use in mid-line mode (no line
            // grab, no trailing newline). After the segments come
            // the line's terminating `\n`, which we emit explicitly
            // since the grammar's "\n" Key isn't preserved in the
            // dict the way `segments` is.
            auto segments_val = dict_value_or_null(*dict, "segments");
            auto segments = std::dynamic_pointer_cast<ArrayValue>(segments_val);
            if (segments) {
                for (const auto& seg : segments->data()) {
                    if (auto s = std::dynamic_pointer_cast<StringValue>(seg)) {
                        if (s->data().empty()) continue;
                        std::size_t origin =
                            locate_item(source, src_cursor, s->data());
                        record_span(parent_span_id, origin,
                                    s->data().size(), out.size(),
                                    "text");
                        out.append(s->data());
                        src_cursor = origin + s->data().size();
                    } else if (auto sd = std::dynamic_pointer_cast<DictValue>(seg)) {
                        auto seg_type = dict_string_or_empty(*sd, "type");
                        if (seg_type == "macro_use") {
                            handle_macro_use(*sd, out, src_cursor,
                                             source, parent_span_id,
                                             /*consume_line=*/false);
                        }
                    }
                }
            }
            // Emit the trailing "\n" the grammar's sibling Key
            // consumed. Source cursor lands at the next line.
            std::size_t nl = source.find('\n', src_cursor);
            if (nl != std::string::npos) {
                out += '\n';
                src_cursor = nl + 1;
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
        if (type == "if") {
            handle_if(*dict, out, src_cursor, source, parent_span_id);
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
// Split `(name1, name2 = default, …)` body text into formals.
// Each formal is `name [= default_text]` per IEEE 1800-2017 §22.5.1.
// Top-level `,` separates formals; `=` (if present) marks the start
// of the default-text run, which continues to the next top-level
// `,` or end of input. Balanced `(` / `)` tracked so a default like
// `ARGS_ = ()` doesn't get half-eaten.
std::vector<MacroParam> split_params(const std::string& body) {
    std::vector<MacroParam> out;
    int depth = 0;
    std::size_t start = 0;
    auto emit = [&](std::size_t end) {
        std::string segment = body.substr(start, end - start);
        MacroParam p;
        auto eq = segment.find('=');
        // First `=` is the default-text delimiter — it can only
        // appear at depth 0 by construction (we only scan here
        // after the depth-tracking scanner has already split on
        // top-level commas). Nested `=` inside `()` stays inside
        // the default_text run.
        if (eq != std::string::npos) {
            p.name = segment.substr(0, eq);
            p.default_text = segment.substr(eq + 1);
            trim_horiz(p.default_text);
        } else {
            p.name = std::move(segment);
        }
        trim_horiz(p.name);
        if (!p.name.empty()) out.push_back(std::move(p));
    };
    for (std::size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (c == ',' && depth == 0) {
            emit(i);
            start = i + 1;
        }
    }
    emit(body.size());
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
                              const std::vector<MacroParam>& params,
                              const std::vector<std::string>& args) {
    std::unordered_map<std::string, std::string> map;
    bool has_params = (params.size() == args.size()) && !params.empty();
    if (has_params) {
        for (std::size_t i = 0; i < params.size(); ++i) {
            std::string arg = args[i];
            trim_horiz(arg);
            map[params[i].name] = std::move(arg);
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

std::string render_segment(const ValuePtr& seg);   // forward decl

// AST-level body substitution. Walks `body_segments` and produces a
// new ArrayValue with parameter references replaced by arg ASTs.
// Substitution rule:
//
//   ref segment in body finds its arg by NAME (the parameter name
//   that the body author wrote in `\`define FOO(x) … x …`), and
//   the arg's POSITION is the index of that name in the params list.
//
// Per segment:
//
//   - DictValue {type:"ref", value:"NAME"} where NAME is a
//     parameter → insert args[idx] verbatim (no string conversion).
//     args[idx] is a ValuePtr — typically StringValue (the current
//     MACRO_ARGS grammar captures identifier args), but can be any
//     AST node a richer args grammar produces. ArrayValue args are
//     spliced flat so multi-token args land as siblings in the
//     body, not as a nested array.
//   - DictValue {type:"ref", value:"…"} where the name isn't a
//     parameter → keep verbatim. It's a non-param identifier
//     reference whose name appears in the expansion as-is.
//   - StringValue (text run, or legacy whole-string body) → run
//     substitute_params text-level: handles `\`"…\`"` stringify
//     and `\`\`` token-paste markers, plus identifier-level param
//     refs in legacy bodies that aren't separately AST-typed.
//   - Other DictValues (macro_use, string literal, etc.) → keep
//     verbatim. render_macro_body_segments handles them — macro_use
//     recurses through render_macro_use_inline, so nested expansion
//     stays in AST land.
//
// `args` is positional; arity-mismatched calls pass an empty list
// from handle_macro_use to disable per-name substitution while
// still firing stringify/paste markers in text segments.
std::shared_ptr<ArrayValue> substitute_segments(
        const ArrayValue& body_segs,
        const std::vector<MacroParam>& params,
        const std::vector<ValuePtr>& args) {
    std::unordered_map<std::string, std::size_t> param_idx;
    bool has_params = (params.size() == args.size()) && !params.empty();
    if (has_params) {
        for (std::size_t i = 0; i < params.size(); ++i) {
            param_idx[params[i].name] = i;
        }
    }

    // Text-segment substitution still wants the args as strings
    // (it does identifier-level token paste / stringify on flat
    // text). Render once here so each text segment doesn't pay the
    // conversion repeatedly. Empty when has_params is false.
    std::vector<std::string> args_text;
    if (has_params) {
        args_text.reserve(args.size());
        for (const auto& a : args) {
            if (auto sv = std::dynamic_pointer_cast<StringValue>(a)) {
                args_text.push_back(sv->data());
            } else {
                args_text.push_back(render_segment(a));
            }
        }
    }

    // Splice an arg AST into `out` — Array args flatten so multi-token
    // args appear inline rather than as a nested array.
    auto splice = [](std::vector<ValuePtr>& out, const ValuePtr& arg) {
        if (auto arr = std::dynamic_pointer_cast<ArrayValue>(arg)) {
            for (const auto& e : arr->data()) out.push_back(e);
        } else {
            out.push_back(arg);
        }
    };

    auto result = std::make_shared<ArrayValue>();
    for (const auto& seg : body_segs.data()) {
        if (auto sv = std::dynamic_pointer_cast<StringValue>(seg)) {
            std::string sub = substitute_params(sv->data(), params, args_text);
            result->data().push_back(make_string(std::move(sub)));
            continue;
        }
        if (auto d = std::dynamic_pointer_cast<DictValue>(seg)) {
            auto type = dict_string_or_empty(*d, "type");
            // `\`\`` paste marker — drops out of the body at
            // substitution time. The neighbouring text/ref segments
            // fuse because no separator survives.
            if (type == "token_paste") continue;
            // `\`"…\`"` stringification — recurse on the inner
            // segments to substitute parameters first, then render
            // the whole thing as a string literal at expand time.
            // Push a marker segment carrying the substituted inner
            // body so render_macro_body_segments can wrap it in `"`.
            if (type == "stringify") {
                auto inner_segs = std::dynamic_pointer_cast<ArrayValue>(
                    dict_value_or_null(*d, "segments"));
                std::shared_ptr<ArrayValue> sub_inner;
                if (inner_segs) {
                    sub_inner = substitute_segments(*inner_segs, params, args);
                } else {
                    sub_inner = std::make_shared<ArrayValue>();
                }
                auto marker = std::make_shared<DictValue>();
                marker->data()["type"] = make_string("stringify");
                marker->data()["segments"] = sub_inner;
                result->data().push_back(marker);
                continue;
            }
            if (type == "ref" && has_params) {
                auto name = dict_string_or_empty(*d, "value");
                auto it = param_idx.find(name);
                if (it != param_idx.end()) {
                    splice(result->data(), args[it->second]);
                    continue;
                }
            }
            // Nested macro_use: substitute outer params into its arg
            // list before the inner call expands. LRM §22.5.1 / C99
            // §6.10.3.1: argument substitution is textual and reaches
            // into the args of nested invocations.
            if (type == "macro_use" && has_params) {
                auto inner_args = std::dynamic_pointer_cast<ArrayValue>(
                    dict_value_or_null(*d, "args"));
                if (inner_args && !inner_args->data().empty()) {
                    auto new_args = std::make_shared<ArrayValue>();
                    for (const auto& a : inner_args->data()) {
                        if (auto sv = std::dynamic_pointer_cast<StringValue>(a)) {
                            auto it = param_idx.find(sv->data());
                            if (it != param_idx.end()) {
                                splice(new_args->data(), args[it->second]);
                                continue;
                            }
                        }
                        new_args->data().push_back(a);
                    }
                    auto new_use = std::make_shared<DictValue>();
                    for (const auto& [k, v] : d->data()) {
                        new_use->data()[k] = (k == "args") ? new_args : v;
                    }
                    result->data().push_back(new_use);
                    continue;
                }
            }
            result->data().push_back(seg);
            continue;
        }
        result->data().push_back(seg);
    }
    return result;
}

// Per IEEE 1800-2017 §22.5.1, a function-like macro call may omit
// trailing arguments whose formals carry a `= default_text` clause.
// If a tail formal has no default, the call is short and the
// caller's arity-mismatch path fires. Returns the padded arg list
// (size == params.size()) on success, or std::nullopt if any
// missing position can't be filled.
template <typename ArgT, typename Make>
std::optional<std::vector<ArgT>> fill_defaults(
        const std::vector<MacroParam>& params,
        std::vector<ArgT> args, Make make_from_default) {
    if (args.size() >= params.size()) return args;
    for (std::size_t i = args.size(); i < params.size(); ++i) {
        if (params[i].default_text.empty()) return std::nullopt;
        args.push_back(make_from_default(params[i].default_text));
    }
    return args;
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

        // AST-level substitution; render walks segments and
        // recurses through any nested macro_use AST entries. args
        // arrived from a text scan of the surrounding body (this
        // is the legacy text-recursion path), so wrap each into
        // a StringValue for the AST substituter.
        std::vector<ValuePtr> args_ast;
        args_ast.reserve(args.size());
        for (const auto& a : args) args_ast.push_back(make_string(a));

        std::shared_ptr<ArrayValue> substituted;
        if (macro.is_function_like
                && macro.params.size() != args_ast.size()) {
            // Short call — try filling missing tail args from
            // each formal's `= default_text` clause.
            auto padded = fill_defaults(macro.params, std::move(args_ast),
                [](const std::string& d) -> ValuePtr {
                    return make_string(d);
                });
            if (padded) {
                args_ast = std::move(*padded);
            }
        }
        if (macro.is_function_like && macro.params.size() != args_ast.size()) {
            state_.warnings.push_back(
                {"macro `" + name + " expects " +
                 std::to_string(macro.params.size()) +
                 " args, got " + std::to_string(args_ast.size()),
                 state_.current_file, state_.current_line});
            substituted = substitute_segments(*macro.body_segments, {}, {});
        } else {
            substituted = substitute_segments(*macro.body_segments,
                                              macro.params, args_ast);
        }

        state_.active_expansions.insert(name);
        ++state_.current_depth;
        std::string expanded = render_macro_body_segments(*substituted);
        --state_.current_depth;
        state_.active_expansions.erase(name);

        out += expanded;
        i = after_args;
    }
    return out;
}

namespace { std::string render_segment(const ValuePtr& seg); }

std::string MacroDef::body_text() const {
    std::string out;
    if (!body_segments) return out;
    for (const auto& seg : body_segments->data()) {
        out += render_segment(seg);
    }
    return out;
}

std::string Preprocessor::render_macro_body_segments(const ArrayValue& segs) {
    std::string out;
    for (const auto& seg : segs.data()) {
        if (auto sv = std::dynamic_pointer_cast<StringValue>(seg)) {
            // Text may carry literal `\`NAME` patterns that
            // weren't AST-typed at parse time (legacy text-only
            // bodies, or expanded-arg text that introduced new
            // macro references). Run them through the text-based
            // scan in expand_recursive so they expand naturally.
            out += expand_recursive(sv->data());
            continue;
        }
        if (auto d = std::dynamic_pointer_cast<DictValue>(seg)) {
            auto type = dict_string_or_empty(*d, "type");
            if (type == "macro_use") {
                out += render_macro_use_inline(*d);
                continue;
            }
            // `\`"…\`"` stringification — wrap the rendered inner
            // body in literal double-quotes. Inner segments have
            // already had parameter substitution applied at
            // substitute_segments time.
            if (type == "stringify") {
                out += '"';
                if (auto inner = std::dynamic_pointer_cast<ArrayValue>(
                        dict_value_or_null(*d, "segments"))) {
                    out += render_macro_body_segments(*inner);
                }
                out += '"';
                continue;
            }
            // ref / string / other typed segments — render leaf.
            out += render_segment(seg);
        }
    }
    return out;
}

std::string Preprocessor::render_macro_use_inline(const DictValue& d) {
    auto name = dict_string_or_empty(d, "name");
    if (name.empty()) return {};

    std::vector<ValuePtr> args;
    if (auto args_val = dict_value_or_null(d, "args")) {
        if (auto arr = std::dynamic_pointer_cast<ArrayValue>(args_val)) {
            for (const auto& a : arr->data()) {
                args.push_back(a);
            }
        }
    }

    auto verbatim = [&]() {
        std::string s = "`" + name;
        if (!args.empty()) {
            s += "(";
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i > 0) s += ",";
                s += render_segment(args[i]);
            }
            s += ")";
        }
        return s;
    };

    // Blue-paint: if this name is already being expanded, emit
    // verbatim to break the cycle.
    if (state_.active_expansions.count(name)) return verbatim();

    auto it = state_.macros.find(name);
    if (it == state_.macros.end()) {
        // Undefined nested use — leave verbatim. The on_undefined
        // policy fires at the source-mapped handle_macro_use entry,
        // not for synthesised inner uses arising from expansion.
        return verbatim();
    }
    const auto& macro = it->second;

    auto args_filled = args;
    if (macro.is_function_like
            && macro.params.size() != args_filled.size()) {
        auto padded = fill_defaults(macro.params, std::move(args_filled),
            [](const std::string& d) -> ValuePtr {
                return make_string(d);
            });
        if (padded) args_filled = std::move(*padded);
        else args_filled = args;
    }

    std::shared_ptr<ArrayValue> substituted;
    if (macro.is_function_like && macro.params.size() != args_filled.size()) {
        state_.warnings.push_back(
            {"macro `" + name + " expects " +
             std::to_string(macro.params.size()) +
             " args, got " + std::to_string(args_filled.size()),
             state_.current_file, state_.current_line});
        substituted = substitute_segments(*macro.body_segments, {}, {});
    } else {
        substituted = substitute_segments(*macro.body_segments,
                                          macro.params, args_filled);
    }

    state_.active_expansions.insert(name);
    ++state_.current_depth;
    std::string result = render_macro_body_segments(*substituted);
    --state_.current_depth;
    state_.active_expansions.erase(name);
    return result;
}

// Render a segmented macro body (ArrayValue produced by
// sv_preprocessor.rawast's scope-array INNERs) back to text.
// Used by handle_define to convert the structured body shape into
// the legacy string form macros expansion machinery already
// consumes. Lossy in one direction (segment-type info is dropped on
// store) but correct: the re-rendered text is what the original
// `\`define` line said. A future expansion path that walks segments
// directly — instead of re-tokenizing the stored string — would
// preserve the structural advantage; for now this keeps the new
// grammar wireable into the existing walker.
namespace {

std::string render_segment(const ValuePtr& seg);

std::string render_macro_args(const ArrayValue& args) {
    std::string out = "(";
    for (std::size_t i = 0; i < args.data().size(); ++i) {
        if (i > 0) out += ',';
        if (auto s = as_string(args.data()[i])) out += s->data();
    }
    out += ')';
    return out;
}

std::string render_segment(const ValuePtr& seg) {
    if (!seg) return {};
    if (auto sv = as_string(seg)) return sv->data();
    auto d = as_dict(seg);
    if (!d) return {};
    auto it = d->data().find("type");
    if (it == d->data().end()) return {};
    auto type_sv = as_string(it->second);
    if (!type_sv) return {};
    const std::string& type = type_sv->data();
    if (type == "ref") {
        if (auto v = d->data().find("value"); v != d->data().end()) {
            if (auto s = as_string(v->second)) return s->data();
        }
    } else if (type == "string") {
        std::string out = "\"";
        if (auto v = d->data().find("value"); v != d->data().end()) {
            if (auto s = as_string(v->second)) out += s->data();
        }
        out += '"';
        return out;
    } else if (type == "macro_use") {
        std::string out = "`";
        if (auto n = d->data().find("name"); n != d->data().end()) {
            if (auto s = as_string(n->second)) out += s->data();
        }
        if (auto a = d->data().find("args"); a != d->data().end()) {
            if (auto arr = as_array(a->second)) out += render_macro_args(*arr);
        }
        return out;
    }
    return {};
}

} // namespace

void Preprocessor::handle_define(const DictValue& d) {
    // Suppressed when walking a not-taken `\`ifdef` branch — the
    // grammar/walker still see the directive (so src_cursor advances
    // and source-map machinery stays consistent), but the macro table
    // is not mutated.
    if (suppress_side_effects_) return;

    MacroDef m;

    // sv_preprocessor.rawast nests name + params under a `decl` field
    // (DECL sub-rule with `ignore:` empty enforces LRM adjacency).
    // Older grammars use flat top-level `name`/`params` — pick whichever
    // shape is present.
    const DictValue* decl = &d;
    if (auto it = d.data().find("decl"); it != d.data().end()) {
        if (auto dd = std::dynamic_pointer_cast<DictValue>(it->second)) {
            decl = dd.get();
        }
    }

    m.name = dict_string_or_empty(*decl, "name");
    if (m.name.empty()) return;

    auto body_it = d.data().find("body");
    if (body_it != d.data().end()) {
        if (auto body_arr = as_array(body_it->second)) {
            if (auto params_it = decl->data().find("params");
                params_it != decl->data().end()) {
                if (auto pa = as_array(params_it->second)) {
                    for (const auto& p : pa->data()) {
                        MacroParam mp;
                        // sv_preprocessor.rawast captures PARAM_FORMAL
                        // as `{name: "…", default: {value: "…"}}`.
                        // Older / synthesized shapes pass a bare
                        // string — read both.
                        if (auto s = as_string(p)) {
                            mp.name = s->data();
                        } else if (auto pd = as_dict(p)) {
                            mp.name = dict_string_or_empty(*pd, "name");
                            if (auto def_v = dict_value_or_null(*pd, "default")) {
                                if (auto dd = as_dict(def_v)) {
                                    mp.default_text =
                                        dict_string_or_empty(*dd, "value");
                                } else if (auto ds = as_string(def_v)) {
                                    mp.default_text = ds->data();
                                }
                            }
                        }
                        if (!mp.name.empty()) m.params.push_back(std::move(mp));
                    }
                    m.is_function_like = true;
                }
            }
            // Strip `\<newline>` line continuations from "literal"
            // segments. The grammar captures them as part of the
            // source text but the macro's logical body should be the
            // joined form. Without this, multi-line `\`define` bodies
            // expand with stray `\` chars that the SV grammar then
            // rejects. Same transform as the legacy-string path
            // below — applied here on a per-segment basis.
            auto strip_continuations = [](std::string in) {
                std::string out;
                out.reserve(in.size());
                for (std::size_t i = 0; i < in.size(); ++i) {
                    if (in[i] == '\\' && i + 1 < in.size()) {
                        if (in[i + 1] == '\n') { out.push_back(' '); ++i; continue; }
                        if (in[i + 1] == '\r') {
                            out.push_back(' ');
                            ++i;
                            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
                            continue;
                        }
                    }
                    out.push_back(in[i]);
                }
                return out;
            };
            auto rewritten = std::make_shared<ArrayValue>();
            for (const auto& seg : body_arr->data()) {
                auto sd = as_dict(seg);
                if (!sd) { rewritten->data().push_back(seg); continue; }
                auto type_it = sd->data().find("type");
                auto val_it = sd->data().find("value");
                bool is_literal = type_it != sd->data().end()
                    && val_it != sd->data().end()
                    && [&]{
                        auto ts = as_string(type_it->second);
                        return ts && ts->data() == "literal";
                    }();
                if (is_literal) {
                    auto vs = as_string(val_it->second);
                    if (vs) {
                        auto new_seg = std::make_shared<DictValue>();
                        for (const auto& [k, v] : sd->data()) {
                            if (k == "value") {
                                new_seg->data()[k] =
                                    make_string(strip_continuations(vs->data()));
                            } else {
                                new_seg->data()[k] = v;
                            }
                        }
                        rewritten->data().push_back(new_seg);
                        continue;
                    }
                }
                rewritten->data().push_back(seg);
            }
            m.body_segments = rewritten;
            state_.macros[m.name] = std::move(m);
            return;
        }
    }

    // Legacy grammars (mini_preprocessor, etc.): `body` is a string;
    // params come from a `(...)` prefix on the body itself. Existing
    // detection logic follows.
    auto raw_body = dict_string_or_empty(d, "body");

    // sv_line_text preserves `\<newline>` continuations literally so
    // the source position machinery can round-trip the original text,
    // but a macro body's logical content is the joined form. Replace
    // each `\\\n` (and `\\\r\n`) with a single space so multi-line
    // bodies expand as one logical sequence. Without this, the `\`
    // chars leak into the output as stray line-continuation markers
    // and the SV grammar rejects them.
    {
        std::string joined;
        joined.reserve(raw_body.size());
        for (std::size_t i = 0; i < raw_body.size(); ++i) {
            if (raw_body[i] == '\\' && i + 1 < raw_body.size()) {
                if (raw_body[i + 1] == '\n') { joined.push_back(' '); ++i; continue; }
                if (raw_body[i + 1] == '\r') {
                    joined.push_back(' ');
                    ++i;
                    if (i + 1 < raw_body.size() && raw_body[i + 1] == '\n') ++i;
                    continue;
                }
            }
            joined.push_back(raw_body[i]);
        }
        raw_body = std::move(joined);
    }

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
            // Legacy text body: wrap in a single-segment array.
            // substitute_segments treats StringValue segments via
            // substitute_params (text-level), so stringification +
            // token-paste + identifier-level param substitution
            // continue to work.
            m.body_segments = std::make_shared<ArrayValue>();
            m.body_segments->data().push_back(make_string(std::move(body)));
            m.is_function_like = true;
            state_.macros[m.name] = std::move(m);
            return;
        }
    }
    // Object-like: body is the captured text verbatim.
    m.body_segments = std::make_shared<ArrayValue>();
    m.body_segments->data().push_back(make_string(std::move(raw_body)));
    m.is_function_like = false;
    state_.macros[m.name] = std::move(m);
}

void Preprocessor::handle_undef(const DictValue& d) {
    if (suppress_side_effects_) return;
    auto name = dict_string_or_empty(d, "name");
    if (!name.empty()) state_.macros.erase(name);
}

void Preprocessor::handle_macro_use(const DictValue& d, std::string& out,
                                    std::size_t& src_cursor,
                                    const std::string& source,
                                    std::uint32_t parent_span_id,
                                    bool consume_line) {
    auto name = dict_string_or_empty(d, "name");
    if (name.empty()) return;

    // Pull args out of the AST as ValuePtrs — the substitution path
    // splices them directly into the body AST at ref positions.
    // For source-mapped emission we also keep a string view of each
    // arg (most are identifier StringValues from MACRO_ARGS, so the
    // string view is the same data without conversion).
    std::vector<ValuePtr> args;
    std::vector<std::string> args_text;
    if (auto args_val = dict_value_or_null(d, "args")) {
        if (auto arr = std::dynamic_pointer_cast<ArrayValue>(args_val)) {
            for (const auto& a : arr->data()) {
                args.push_back(a);
                if (auto s = std::dynamic_pointer_cast<StringValue>(a)) {
                    args_text.push_back(s->data());
                } else {
                    args_text.push_back(render_segment(a));
                }
            }
        }
    }

    // Locate the `\`NAME` site in the source. PP_MACRO_USE consumes
    // a full line (including the trailing newline via sv_eol). For
    // function-like macros the `(...)` argument list may span several
    // physical lines (sv_balanced_arg tracks paren depth), so we
    // must balance-scan past the closing `)` before looking for the
    // line terminator — otherwise scan_past_directive_line stops at
    // the first embedded newline and the macro's tail lines get
    // double-emitted on the next walker step.
    std::size_t use_src_start = locate_item(source, src_cursor, "`" + name);
    std::size_t after_name = use_src_start + 1 + name.size();
    std::size_t arg_scan = after_name;
    while (arg_scan < source.size()
           && (source[arg_scan] == ' ' || source[arg_scan] == '\t')) {
        ++arg_scan;
    }
    if (!args.empty() && arg_scan < source.size() && source[arg_scan] == '(') {
        int depth = 0;
        std::size_t p = arg_scan;
        while (p < source.size()) {
            char c = source[p++];
            if (c == '(') ++depth;
            else if (c == ')') {
                if (--depth == 0) break;
            }
        }
        after_name = p;
    }
    // Line-based callers (mini_preprocessor, top-level PP_FILE) want
    // `\`MACRO` to own the trailing newline so handle_macro_use leaves
    // the cursor on the next line. For mid-text use surfaced by the
    // sv_preprocessor.rawast text_line iterator, the macro call sits
    // inside surrounding bytes — consuming the newline would lose any
    // text after the macro on the same line.
    std::size_t use_src_end = consume_line
        ? scan_past_directive_line(source, after_name)
        : after_name;
    std::size_t use_src_len = use_src_end - use_src_start;
    bool consumed_newline = consume_line
        && use_src_end > use_src_start
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
        auto args_filled = args;
        if (macro.is_function_like
                && macro.params.size() != args_filled.size()) {
            auto padded = fill_defaults(macro.params, std::move(args_filled),
                [](const std::string& d) -> ValuePtr {
                    return make_string(d);
                });
            if (padded) args_filled = std::move(*padded);
            else args_filled = args;
        }
        std::shared_ptr<ArrayValue> substituted;
        if (macro.is_function_like
                && macro.params.size() != args_filled.size()) {
            // Arity mismatch — warn; still substitute (with empty
            // param/arg lists) so `\`"…\`"` stringification and
            // `\`\`` token-paste markers fire regardless.
            state_.warnings.push_back(
                {"macro `" + name + " expects " +
                 std::to_string(macro.params.size()) +
                 " args, got " + std::to_string(args_filled.size()),
                 state_.current_file, state_.current_line});
            substituted = substitute_segments(*macro.body_segments, {}, {});
        } else {
            // Matching arity OR object-like (no params). Empty
            // params means identifier-shaped refs in text segments
            // pass through but stringify / paste markers still
            // fire, and typed `ref` segments stay verbatim (which
            // is what we want — they were never parameter refs).
            substituted = substitute_segments(*macro.body_segments,
                                              macro.params, args_filled);
        }
        // Render the substituted segments. macro_use segments
        // recurse through render_macro_use_inline — no string
        // round-trip, no re-parse. Blue-paint guard prevents
        // `\`A`-in-`\`A`-body infinite recursion.
        state_.active_expansions.insert(name);
        ++state_.current_depth;
        std::string emitted = render_macro_body_segments(*substituted);
        --state_.current_depth;
        state_.active_expansions.erase(name);
        emit_expansion(emitted, "macro " + name + " expansion");
        src_cursor = use_src_end;
        return;
    }

    // Undefined — give the host's undefined_handler first refusal.
    // If it returns a value, that's the expansion (recursively
    // re-expanded so the host can return text containing macro
    // uses). If it returns nullopt, fall through to the static
    // on_undefined policy.
    if (opts_.undefined_handler) {
        if (auto replacement = opts_.undefined_handler(name, args_text)) {
            state_.active_expansions.insert(name);
            ++state_.current_depth;
            std::string emitted = expand_recursive(*replacement);
            --state_.current_depth;
            state_.active_expansions.erase(name);
            emit_expansion(emitted,
                            "undefined_handler expansion of " + name);
            src_cursor = use_src_end;
            return;
        }
    }

    auto leave_emit = [&]() {
        std::string emit = "`" + name;
        if (!args_text.empty()) {
            emit += "(";
            for (std::size_t i = 0; i < args_text.size(); ++i) {
                if (i > 0) emit += ",";
                emit += args_text[i];
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

    // Suppressed inside a not-taken `\`ifdef` branch: the directive
    // line was already skipped via src_cursor advance above; we just
    // need to NOT actually open / parse / walk the included file
    // (which would mutate the macro table and included_files).
    if (suppress_side_effects_) return;

    std::string canonical;
    std::string include_text;

    // Host-supplied include source takes priority. If the callback
    // is set and returns a value, we use its (canonical_id, content)
    // directly — no filesystem walk needed. nullopt means "I can't
    // resolve this, try the built-in fallback."
    if (opts_.include_source) {
        auto host = opts_.include_source(path, state_.current_file);
        if (host) {
            canonical    = std::move(host->canonical_id);
            include_text = std::move(host->content);
        }
    }

    // Built-in fallback: walk `include_paths`, then the including
    // file's directory, then the path as given. Engaged when no
    // callback was set, or when the callback returned nullopt.
    if (canonical.empty() && include_text.empty()) {
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

        canonical = resolved.lexically_normal().string();

        std::ifstream f(resolved);
        if (!f.is_open()) {
            state_.warnings.push_back(
                {"`include: failed to open '" + canonical + "'",
                 state_.current_file, state_.current_line});
            return;
        }
        std::ostringstream buf;
        buf << f.rdbuf();
        include_text = buf.str();
    }

    // `included_files` is a deduped manifest used by build systems
    // for incremental-rebuild dependency tracking. Same logical
    // source included multiple times (e.g. with redefines between
    // them) lists once. This does NOT gate processing — every
    // `\`include` is parsed + walked freshly with the current macro
    // table; the dedup is purely about the query result.
    bool already_seen = false;
    for (const auto& p : state_.included_files) {
        if (p == canonical) { already_seen = true; break; }
    }
    if (!already_seen) state_.included_files.push_back(canonical);

    // Parse the included file with the same preprocessor grammar.
    // (No depth-of-includes check yet — Phase 3 polish.)
    auto include_stream = Stream::from_string(include_text);
    auto parsed = pp_grammar_.parse(include_stream);
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
            // Suppress state mutations (macro register/erase,
            // included_files push) for the not-taken branch — SV LRM
            // says directives inside an untaken `\`ifdef` don't
            // execute. Save and restore on top of any outer suppress
            // already in effect (nested ifdef inside a skipped block
            // must stay suppressed regardless of inner take/skip).
            bool saved_suppress = suppress_side_effects_;
            suppress_side_effects_ = true;
            walk(branch, discard, src_cursor, source, parent_span_id);
            suppress_side_effects_ = saved_suppress;
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

void Preprocessor::handle_if(const DictValue& d, std::string& out,
                              std::size_t& src_cursor,
                              const std::string& source,
                              std::uint32_t parent_span_id) {
    // Branches AST shape (flat):
    //   { type:"if",
    //     branches: [ {cond:"EXPR", body:[...]},
    //                 {cond:"EXPR", body:[...]} ],
    //     else_branch: [...] }
    // The first `if` is branches[0]; subsequent entries are `elsif`s.
    // Grammar producers normalize their nested `\`if/\`elsif/\`else/
    // \`endif` syntax into this shape so the walker stays linear.

    src_cursor = locate_item(source, src_cursor, "`if");
    src_cursor = scan_past_directive_line(source, src_cursor);

    auto walk_or_discard = [&](const ValuePtr& branch, bool emit) {
        if (!branch) return;
        if (emit) {
            walk(branch, out, src_cursor, source, parent_span_id);
        } else {
            std::string discard;
            std::size_t saved = state_.spans.size();
            // Same side-effect suppression as handle_ifdef — not-taken
            // branches must not register/erase macros or push includes.
            bool saved_suppress = suppress_side_effects_;
            suppress_side_effects_ = true;
            walk(branch, discard, src_cursor, source, parent_span_id);
            suppress_side_effects_ = saved_suppress;
            state_.spans.resize(saved);
        }
    };

    bool any_taken = false;
    if (auto branches_val = dict_value_or_null(d, "branches")) {
        if (auto arr = std::dynamic_pointer_cast<ArrayValue>(branches_val)) {
            std::size_t branch_idx = 0;
            for (const auto& b : arr->data()) {
                auto bd = std::dynamic_pointer_cast<DictValue>(b);
                if (!bd) continue;
                auto cond = dict_value_or_null(*bd, "cond");
                // sv_preprocessor.rawast captures cond with `*:cond=@`
                // (Raw). Raw bypasses the ignore-set, so the leading
                // space between the keyword and the expression lands
                // inside cond. Trim before handing to expr_eval.
                if (auto sv = std::dynamic_pointer_cast<StringValue>(cond)) {
                    const auto& s = sv->data();
                    std::size_t i = 0;
                    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
                    std::size_t j = s.size();
                    while (j > i && (s[j-1] == ' ' || s[j-1] == '\t')) --j;
                    if (i > 0 || j < s.size()) {
                        cond = make_string(s.substr(i, j - i));
                    }
                }
                bool take = false;
                if (!opts_.expr_eval) {
                    state_.warnings.push_back(
                        {"`if encountered without expr_eval callback; "
                         "treating branch as false",
                         state_.current_file, state_.current_line});
                } else if (auto v = opts_.expr_eval(cond)) {
                    take = *v;
                } else {
                    state_.warnings.push_back(
                        {"expr_eval could not evaluate `if condition; "
                         "treating as false",
                         state_.current_file, state_.current_line});
                }
                // Position past `\`elsif` line for branches beyond the
                // first — only walks if the first branch hasn't
                // already taken; otherwise the locator scan will
                // skip ahead through suppressed text.
                if (branch_idx > 0) {
                    src_cursor = locate_item(source, src_cursor, "`elsif");
                    src_cursor = scan_past_directive_line(source, src_cursor);
                }
                bool emit = take && !any_taken;
                walk_or_discard(dict_value_or_null(*bd, "body"), emit);
                if (take) any_taken = true;
                ++branch_idx;
            }
        }
    }

    if (auto eb = dict_value_or_null(d, "else_branch")) {
        src_cursor = locate_item(source, src_cursor, "`else");
        src_cursor = scan_past_directive_line(source, src_cursor);
        walk_or_discard(eb, !any_taken);
    }

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
