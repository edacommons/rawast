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
            walk(child, out, src_cursor, source, parent_span_id);
        }
        return;
    }

    if (auto dict = std::dynamic_pointer_cast<DictValue>(v)) {
        auto type = dict_string_or_empty(*dict, "type");

        if (type == "text") {
            auto text = dict_string_or_empty(*dict, "text");
            if (!text.empty()) {
                // Find this captured text in source from cursor.
                // The grammar's ignore policy may have skipped some
                // leading whitespace; the find() catches that and
                // gives us the true source offset.
                std::size_t origin = locate_item(source, src_cursor, text);
                record_span(parent_span_id, origin,
                            text.size(), out.size(), "text");
                out += text;
                src_cursor = origin + text.size();
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

void Preprocessor::handle_macro_use(const DictValue& d, std::string& out,
                                    std::size_t& src_cursor,
                                    const std::string& source,
                                    std::uint32_t parent_span_id) {
    auto name = dict_string_or_empty(d, "name");
    if (name.empty()) return;

    // Source span for the entire macro-use site (e.g. `\`UVM_INFO`).
    // For Phase 2.0 we approximate as `\`` + name (no args yet —
    // function-like macro uses arrive in Phase 2.1; we'll widen
    // the span to cover the arg list when those land).
    std::size_t use_src_start = src_cursor;
    std::size_t use_src_len = 1 + name.size();  // backtick + name

    auto it = state_.macros.find(name);
    if (it != state_.macros.end()) {
        // Object-like expansion: emit body verbatim. Provenance
        // for the emitted bytes points at the call site (the
        // user-facing convention — errors blame the call, not the
        // body). Phase 2.1 will introduce a body span as parent
        // so the full expansion chain is preserved.
        const auto& body = it->second.body;
        if (!body.empty()) {
            record_span(parent_span_id, use_src_start,
                        use_src_len, out.size(),
                        "macro " + name + " expansion");
            out += body;
        }
        src_cursor = use_src_start + use_src_len;
        return;
    }
    // Undefined — apply policy.
    switch (opts_.on_undefined) {
        case PpOnUndefined::Leave: {
            record_span(parent_span_id, use_src_start,
                        use_src_len, out.size(),
                        "undefined macro use");
            out += "`";
            out += name;
            src_cursor = use_src_start + use_src_len;
            return;
        }
        case PpOnUndefined::Empty:
            src_cursor = use_src_start + use_src_len;
            return;
        case PpOnUndefined::Warn:
            state_.warnings.push_back(
                {"undefined macro `" + name, state_.current_file,
                 state_.current_line});
            record_span(parent_span_id, use_src_start,
                        use_src_len, out.size(),
                        "undefined macro use");
            out += "`";
            out += name;
            src_cursor = use_src_start + use_src_len;
            return;
        case PpOnUndefined::Error:
            throw std::runtime_error(
                "undefined macro `" + name);
    }
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
