#include <rawast/preprocessor.hpp>
#include <functional>

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

std::string_view to_string(PpOnUndecidable v) noexcept {
    switch (v) {
        case PpOnUndecidable::TreatAsFalse: return "false";
        case PpOnUndecidable::TreatAsTrue:  return "true";
        case PpOnUndecidable::Error:        return "error";
    }
    return "";
}

std::optional<PpOnUndecidable> parse_pp_on_undecidable(std::string_view name) noexcept {
    if (name == "false") return PpOnUndecidable::TreatAsFalse;
    if (name == "true")  return PpOnUndecidable::TreatAsTrue;
    if (name == "error") return PpOnUndecidable::Error;
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

} // namespace

std::string Preprocessor::process(const std::string& text) {
    // A grammar with no top rule (degenerate skeleton) can't recognise
    // constructs — pass text through unchanged.
    if (!pp_grammar_.top().valid()) return text;
    state_.spans.clear();
    std::string out;
    scan_stream(text, out);
    return out;
}
// scan_stream / expand_macro_use are defined lower down, after the
// file-local expansion helpers (trim_horiz, render_segment, …) they use.

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

// Escape a string for embedding inside a `\`"…`"` stringification literal:
// `"` and `\` are backslash-escaped (IEEE 1800-2017 §22.5.1). Without this a
// stringified expression that itself contains a string/comment (e.g. a
// `DV_CHECK` condition `foo() // note "x"`) produces an unbalanced literal.
inline std::string stringify_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

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
        const std::vector<ValuePtr>& args_in) {
    // Trim leading/trailing whitespace from each call-site arg before
    // substituting. The MACRO_ARGS grammar scope captures bytes
    // verbatim, which preserves the call site's line breaks and
    // indentation — and when an arg lands at a `\`\`` token-paste
    // boundary the preserved whitespace prevents fusion.
    //
    // Example: `\`DV_CHECK_EQ(status, UVM_IS_OK,\n    error, ...)`
    // The SEV_ arg captured literally is "\n    error". Pasting
    // `dv_` + "\n    error" yields "`dv_\n    error" — a broken
    // identifier with a newline mid-name. Standard SV preprocessor
    // convention (LRM §22.5.1) treats macro args as token sequences,
    // not whitespace-preserving text, so trim here.
    //
    // After trim, any empty positional arg that corresponds to a
    // formal with a default text falls back to the default. The LRM
    // is ambiguous on intermediate empty args (`\`MAC(a, , c)`) but
    // most simulators (VCS / Verilator) treat them as "not specified"
    // and use the default. Without this, `\`DV_WAIT_TIMEOUT(, , msg)`
    // would substitute TIMEOUT_NS_ to "" and emit `#( * 1ns)` — the
    // empty arg leaks into the body's `#(TIMEOUT_NS_ * 1ns)` slot.
    auto trim_ws = [](const std::string& s) -> std::string {
        std::size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return s.substr(b, e - b);
    };
    std::vector<ValuePtr> args;
    args.reserve(args_in.size());
    for (std::size_t i = 0; i < args_in.size(); ++i) {
        const auto& a = args_in[i];
        if (auto sv = std::dynamic_pointer_cast<StringValue>(a)) {
            std::string trimmed = trim_ws(sv->data());
            if (trimmed.empty()
                    && i < params.size()
                    && !params[i].default_text.empty()) {
                args.push_back(make_string(trim_ws(params[i].default_text)));
            } else {
                args.push_back(make_string(std::move(trimmed)));
            }
        } else {
            args.push_back(a);
        }
    }

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
    const auto& segdata = body_segs.data();
    for (std::size_t si = 0; si < segdata.size(); ++si) {
        const auto& seg = segdata[si];
        if (auto sv = std::dynamic_pointer_cast<StringValue>(seg)) {
            std::string sub = substitute_params(sv->data(), params, args_text);
            result->data().push_back(make_string(std::move(sub)));
            continue;
        }
        if (auto d = std::dynamic_pointer_cast<DictValue>(seg)) {
            auto type = dict_string_or_empty(*d, "type");
            // `\`\`` token paste (IEEE 1800-2017 §22.5.1): fuse the
            // previously-emitted token with the NEXT segment into ONE
            // token. When the left operand is a macro_use (e.g. `\`dv_`),
            // the fused spelling names a macro to expand — `\`dv_``SEV_`
            // with SEV_="fatal" is `\`dv_fatal`, NOT a `\`dv_` use next to
            // the text "fatal". We resolve it in the segment domain:
            // build `\`<name>`, pull any following call `(args)` in with
            // it, and emit that as one string, which
            // render_macro_body_segments re-scans through expand_recursive
            // — so the constructed macro use expands like any other.
            if (type == "token_paste") {
                if (result->data().empty() || si + 1 >= segdata.size())
                    continue;
                // Spelling of the LEFT operand (already emitted).
                ValuePtr left = result->data().back();
                result->data().pop_back();
                std::string left_txt;
                bool left_is_macro = false;
                if (auto ld = std::dynamic_pointer_cast<DictValue>(left)) {
                    if (dict_string_or_empty(*ld, "type") == "macro_use") {
                        left_is_macro = true;
                        left_txt = dict_string_or_empty(*ld, "name");
                    } else {
                        left_txt = render_segment(left);
                    }
                } else if (auto ls =
                               std::dynamic_pointer_cast<StringValue>(left)) {
                    left_txt = ls->data();
                }
                // Spelling of a segment, with outer-param substitution.
                // Recursive so a `\`"…`"` stringify operand resolves its
                // inner ref against the outer arg BEFORE being quoted —
                // otherwise a paste-absorbed call arg like `\`"T_`"` renders
                // empty and leaves a `f(a, , b)` hole.
                std::function<std::string(const ValuePtr&)> spell =
                        [&](const ValuePtr& s) -> std::string {
                    if (auto rd = std::dynamic_pointer_cast<DictValue>(s)) {
                        auto st = dict_string_or_empty(*rd, "type");
                        if (st == "ref") {
                            auto rn = dict_string_or_empty(*rd, "value");
                            auto pit = param_idx.find(rn);
                            if (pit != param_idx.end())
                                return render_segment(args[pit->second]);
                            return rn;
                        }
                        if (st == "stringify") {
                            std::string inner;
                            if (auto segs = std::dynamic_pointer_cast<ArrayValue>(
                                    dict_value_or_null(*rd, "segments"))) {
                                for (const auto& is : segs->data())
                                    inner += spell(is);
                            }
                            return "\"" + stringify_escape(inner) + "\"";
                        }
                        return render_segment(s);
                    }
                    if (auto rs = std::dynamic_pointer_cast<StringValue>(s))
                        return substitute_params(rs->data(), params, args_text);
                    return {};
                };
                std::string right_txt = spell(segdata[si + 1]);
                ++si;  // consumed the right operand
                std::string fused = left_txt + right_txt;
                if (!left_is_macro) {
                    // Plain identifier paste — a fused text token.
                    result->data().push_back(make_string(std::move(fused)));
                    continue;
                }
                // Constructed macro use. Absorb a following balanced
                // `(...)` call-argument run so the emitted string is a
                // complete function-like invocation.
                std::string call = "`" + fused;
                if (si + 1 < segdata.size()) {
                    std::string peek = spell(segdata[si + 1]);
                    std::size_t nb = peek.find_first_not_of(" \t");
                    if (nb != std::string::npos && peek[nb] == '(') {
                        int depth = 0;
                        std::size_t j = si + 1;
                        for (; j < segdata.size(); ++j) {
                            std::string t = spell(segdata[j]);
                            for (char c : t) {
                                if (c == '(') ++depth;
                                else if (c == ')') --depth;
                            }
                            call += t;
                            if (depth <= 0) break;
                        }
                        si = j;  // consumed through the closing ')'
                    }
                }
                result->data().push_back(make_string(std::move(call)));
                continue;
            }
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
            // into the args of nested invocations. An inner arg may
            // be either:
            //   - a bare parameter name (`\`MAC(__sig)`) → splice the
            //     outer arg ValuePtr directly so non-string types
            //     survive
            //   - any other text run (`\`MAC(!$isunknown(__sig))`) →
            //     run `substitute_params` over the arg text so
            //     parameter references inside the arg are replaced
            //     and stringification/paste markers fire
            if (type == "macro_use" && has_params) {
                auto inner_args = std::dynamic_pointer_cast<ArrayValue>(
                    dict_value_or_null(*d, "args"));
                if (inner_args && !inner_args->data().empty()) {
                    auto new_args = std::make_shared<ArrayValue>();
                    for (const auto& a : inner_args->data()) {
                        if (auto sv = std::dynamic_pointer_cast<StringValue>(a)) {
                            // Whole-arg matches a param name → splice.
                            auto it = param_idx.find(sv->data());
                            if (it != param_idx.end()) {
                                splice(new_args->data(), args[it->second]);
                                continue;
                            }
                            // Otherwise textual substitution inside.
                            std::string sub = substitute_params(
                                sv->data(), params, args_text);
                            new_args->data().push_back(make_string(std::move(sub)));
                            continue;
                        }
                        // Non-string inner arg — a `\`"…\`"` stringify dict
                        // or a further nested macro_use. Recursively
                        // substitute the OUTER params into it so a
                        // stringify operand / nested ref resolves to the
                        // outer arg BEFORE the inner call expands. Without
                        // this, `\`uvm_record_int(\`"ARG\`", …)` keeps the
                        // literal "ARG" (the UVM field-automation leak).
                        auto one = std::make_shared<ArrayValue>();
                        one->data().push_back(a);
                        auto subbed = substitute_segments(*one, params, args);
                        for (const auto& e : subbed->data())
                            new_args->data().push_back(e);
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
        // Function-like: the `(args)` may follow after LRM §22.5.1
        // whitespace. Whether the `(...)` is arguments is decided HERE
        // (we know `is_function_like` from the table) — an object-like
        // macro leaves any following `(...)` as text.
        if (macro.is_function_like) {
            std::size_t paren = name_end;
            while (paren < text.size()
                   && (text[paren] == ' ' || text[paren] == '\t')) ++paren;
            if (paren < text.size() && text[paren] == '(') {
                after_args = scan_args(text, paren, args);
                if (after_args == paren) {
                    // Unbalanced parens — give up on this expansion.
                    out.append(text, i, name_end - i);
                    i = name_end;
                    continue;
                }
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

    // Nested conditional directives that may appear inside an
    // expanded macro body — e.g. `\`ASSERT_ERROR` wraps its body in
    // `\`ifdef UVM … \`else … \`endif`. The rendered text MUST
    // resolve them against the current macro table rather than
    // leaving raw `\`ifdef` tokens in the host stream; otherwise
    // the SV grammar chokes on a stray `\`ifdef` mid-statement. Each
    // stack entry is "taking" for the current branch; AND of the
    // stack gates output. Per IEEE 1800-2017 §22.5.1.
    std::vector<bool> cond_stack;
    auto cond_taking = [&]() {
        for (bool b : cond_stack) if (!b) return false;
        return true;
    };

    const auto& data = segs.data();
    for (std::size_t k = 0; k < data.size(); ++k) {
        const auto& seg = data[k];

        // Directive macro_use segments (`\`ifdef`/`\`ifndef`/
        // `\`else`/`\`endif`) — handled BEFORE the generic macro_use
        // path so they never reach render_macro_use_inline (which
        // would render them verbatim as undefined-macro tokens).
        // `\`ifdef X` parses as two segments: {macro_use, name:"ifdef"}
        // followed by {ref, value:"X"} (the condition name).
        if (auto d = std::dynamic_pointer_cast<DictValue>(seg)) {
            auto type = dict_string_or_empty(*d, "type");
            if (type == "macro_use") {
                auto name = dict_string_or_empty(*d, "name");
                if (name == "ifdef" || name == "ifndef") {
                    std::string cond;
                    // The cond ref follows the directive, possibly after a
                    // whitespace-only text segment (PP_MACRO_USE is tight
                    // and no longer swallows the separating space). Skip
                    // such blank segments to reach the `ref`.
                    std::size_t j = k + 1;
                    while (j < data.size()) {
                        if (auto sv = std::dynamic_pointer_cast<StringValue>(
                                data[j])) {
                            bool blank = sv->data().find_first_not_of(" \t")
                                         == std::string::npos;
                            if (blank) { ++j; continue; }
                        }
                        break;
                    }
                    if (j < data.size()) {
                        if (auto dn = std::dynamic_pointer_cast<DictValue>(
                                data[j])) {
                            auto nt = dict_string_or_empty(*dn, "type");
                            if (nt == "ref") {
                                cond = dict_string_or_empty(*dn, "value");
                                k = j;
                            }
                        }
                    }
                    bool defined = !cond.empty()
                        && state_.macros.find(cond) != state_.macros.end();
                    bool take = (name == "ifdef") ? defined : !defined;
                    cond_stack.push_back(cond_taking() && take);
                    continue;
                }
                if (name == "else") {
                    if (!cond_stack.empty()) {
                        bool outer_taking = true;
                        for (std::size_t j = 0; j + 1 < cond_stack.size(); ++j) {
                            if (!cond_stack[j]) { outer_taking = false; break; }
                        }
                        cond_stack.back() = outer_taking && !cond_stack.back();
                    }
                    continue;
                }
                if (name == "endif") {
                    if (!cond_stack.empty()) cond_stack.pop_back();
                    continue;
                }
            }
        }

        if (!cond_taking()) continue;

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
                // If NAME is function-like but no args were captured, a
                // space separated it from `(` — the args sit in the
                // FOLLOWING segments (`\`INC (Y)`). Splice `\`NAME + the
                // rendered tail and let expand_recursive attach them
                // table-aware (it knows is_function_like); this consumes
                // the rest of the body as text, fine for trailing content
                // after the call. Object-like uses and captured-args uses
                // fall through to the isolated render below.
                auto nm = dict_string_or_empty(*d, "name");
                bool has_args = false;
                if (auto a = d->data().find("args"); a != d->data().end())
                    if (auto arr = as_array(a->second))
                        has_args = !arr->data().empty();
                auto mit = state_.macros.find(nm);
                bool fn_like = mit != state_.macros.end()
                               && mit->second.is_function_like;
                if (fn_like && !has_args && k + 1 < data.size()) {
                    std::string tail;
                    for (std::size_t j = k + 1; j < data.size(); ++j)
                        tail += render_segment(data[j]);
                    std::size_t p = 0;
                    while (p < tail.size()
                           && (tail[p] == ' ' || tail[p] == '\t')) ++p;
                    if (p < tail.size() && tail[p] == '(') {
                        out += expand_recursive("`" + nm + tail);
                        k = data.size();   // consumed remainder as text
                        continue;
                    }
                }
                out += render_macro_use_inline(*d);
                continue;
            }
            // `\`"…\`"` stringification — wrap the rendered inner
            // body in literal double-quotes. Inner segments have
            // already had parameter substitution applied at
            // substitute_segments time.
            if (type == "stringify") {
                std::string inner_text;
                if (auto inner = std::dynamic_pointer_cast<ArrayValue>(
                        dict_value_or_null(*d, "segments"))) {
                    inner_text = render_macro_body_segments(*inner);
                }
                out += '"';
                out += stringify_escape(inner_text);
                out += '"';
                continue;
            }
            // ref / string / other typed segments — render leaf.
            out += render_segment(seg);
        }
    }

    // Strip `//` line comments from the rendered body. Line comments
    // in multi-line macro bodies survive the define-time strip when
    // they straddle segment boundaries (the `\<newline>` LINE_CONT
    // consumes the newline, splitting `// comment text` across two
    // StringValue segments with no `\n` to anchor the strip). After
    // the body is fully rendered into one string, a single pass
    // catches them. Block comments and string-quoted `//` are
    // respected. Per common SV preprocessor convention (VCS,
    // Verilator, Icarus).
    {
        std::string stripped;
        stripped.reserve(out.size());
        bool in_string = false;
        for (std::size_t i = 0; i < out.size(); ++i) {
            char c = out[i];
            if (in_string) {
                stripped.push_back(c);
                if (c == '\\' && i + 1 < out.size()) {
                    stripped.push_back(out[++i]);
                    continue;
                }
                if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') { in_string = true; stripped.push_back(c); continue; }
            if (c == '/' && i + 1 < out.size() && out[i + 1] == '/') {
                while (i < out.size() && out[i] != '\n') ++i;
                if (i < out.size()) stripped.push_back(' ');
                continue;
            }
            stripped.push_back(c);
        }
        out = std::move(stripped);
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

    // Object-like macros take NO arguments: a `(...)` after the name is
    // following TEXT, not an argument list (the grammar captured it as
    // args syntactically, but only the table knows the macro is
    // object-like — LRM §22.5.1). Expand the body, then re-emit the
    // parens as text: `\`W(3)` with `\`define W 8` → `8(3)`, not `8`.
    if (!macro.is_function_like && !args.empty()) {
        state_.active_expansions.insert(name);
        ++state_.current_depth;
        std::string body = render_macro_body_segments(
            *substitute_segments(*macro.body_segments, {}, {}));
        --state_.current_depth;
        state_.active_expansions.erase(name);
        body += "(";
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) body += ",";
            body += render_segment(args[i]);
        }
        body += ")";
        return body;
    }

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
// systemverilog.rawast's scope-array INNERs) back to text.
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
    // The scan driver only calls this in an emitting branch, so a
    // not-taken `\`ifdef` never reaches here — no suppression guard
    // needed.
    MacroDef m;

    // systemverilog.rawast nests name + params under a `decl` field
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
                        // systemverilog.rawast captures PARAM_FORMAL
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
            // Strip `\<newline>` line continuations from literal body
            // segments. The grammar captures them as part of the
            // source text but the macro's logical body is the joined
            // form. Line comments are handled separately below
            // (segment-aware skip until next LINE_CONT boundary) so
            // we don't strip `//` here — doing so would hide it from
            // the `find("//")` check used to trigger comment-mode.
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
            // Recursive form: strip `\<newline>` from EVERY string in a
            // value tree. Needed for nested macro-call segments (a
            // `macro_use` dict) whose captured arguments may span
            // continuation lines — e.g. `\`C(a, \<newline> b)` inside a
            // macro body. The flat strip above only reaches bare-string
            // and `literal`-dict segments, so without this the `\` leaks
            // into the nested call's args and survives expansion.
            std::function<ValuePtr(const ValuePtr&)> strip_deep =
                [&](const ValuePtr& v) -> ValuePtr {
                    if (!v) return v;
                    if (auto vs = as_string(v))
                        return make_string(strip_continuations(vs->data()));
                    if (auto arr = as_array(v)) {
                        auto out = std::make_shared<ArrayValue>();
                        for (const auto& e : arr->data())
                            out->data().push_back(strip_deep(e));
                        return out;
                    }
                    if (auto dd = as_dict(v)) {
                        auto out = std::make_shared<DictValue>();
                        for (const auto& [k, val] : dd->data())
                            out->data().emplace(k, strip_deep(val));
                        return out;
                    }
                    return v;
                };
            // The sv_preprocessor body scope emits literal text as
            // bare StringValue segments interleaved with typed dicts
            // (macro_use / ref / string / stringify / token_paste) and
            // null entries (LINE_CONT matches at source-line ends).
            //
            // Apply two transforms here:
            //   1. strip_continuations to bare StringValues (and the
            //      legacy `{type:"literal", value:"…"}` shape) — the
            //      body's logical content is the joined form.
            //   2. drop every segment from the FIRST `//` in a bare
            //      StringValue up to (but not including) the next
            //      null entry, which marks the next LINE_CONT boundary
            //      (i.e. the source-line break). The `//` token would
            //      otherwise survive into the expanded body and, once
            //      LINE_CONTs are joined, swallow everything until the
            //      next host-stream `\n` — including subsequent body
            //      content and trailing host code.
            auto rewritten = std::make_shared<ArrayValue>();
            bool skip_until_line_end = false;
            for (const auto& seg : body_arr->data()) {
                // null = LINE_CONT match (no save fields) marking a
                // source-line break. Always emit it to preserve the
                // segment-array shape, and reset comment-skip state.
                if (!seg) {
                    rewritten->data().push_back(seg);
                    skip_until_line_end = false;
                    continue;
                }
                if (skip_until_line_end) continue;
                if (auto vs = as_string(seg)) {
                    auto joined = strip_continuations(vs->data());
                    auto slash = joined.find("//");
                    if (slash != std::string::npos) {
                        joined.resize(slash);
                        skip_until_line_end = true;
                    }
                    rewritten->data().push_back(make_string(std::move(joined)));
                    continue;
                }
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
                // Other typed dict (macro_use / ref / string / stringify
                // / token_paste): deep-strip continuations from its
                // captured text (notably a nested call's multi-line args).
                rewritten->data().push_back(strip_deep(seg));
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
    //
    // Line comments (`// ...`) inside multi-line macro bodies are
    // stripped here too: once `\<newline>` continuations are joined,
    // any `//` in the body would run to the next *output*-stream `\n`
    // (which comes from the macro INVOCATION's surrounding context),
    // swallowing both the rest of the macro body and trailing host
    // code. SV preprocessor convention (matching VCS, Verilator,
    // Icarus) is to strip `//` comments from the body at define-time
    // so the expanded text contains the actual macro content rather
    // than collapsing into one big comment.
    //
    // Block comments `/* ... */` are not stripped: they don't have
    // the line-terminated swallow problem.
    //
    // String literals are respected so `"//"` inside a string doesn't
    // trigger comment stripping.
    {
        std::string joined;
        joined.reserve(raw_body.size());
        bool in_string = false;
        for (std::size_t i = 0; i < raw_body.size(); ++i) {
            char c = raw_body[i];
            if (in_string) {
                joined.push_back(c);
                if (c == '\\' && i + 1 < raw_body.size()) {
                    joined.push_back(raw_body[++i]);
                    continue;
                }
                if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') { in_string = true; joined.push_back(c); continue; }
            if (c == '\\' && i + 1 < raw_body.size()) {
                if (raw_body[i + 1] == '\n') { joined.push_back(' '); ++i; continue; }
                if (raw_body[i + 1] == '\r') {
                    joined.push_back(' ');
                    ++i;
                    if (i + 1 < raw_body.size() && raw_body[i + 1] == '\n') ++i;
                    continue;
                }
            }
            if (c == '/' && i + 1 < raw_body.size() && raw_body[i + 1] == '/') {
                // Skip until the next unescaped newline. A `\<newline>`
                // inside the comment is part of the source-line
                // continuation, not the comment terminator — but since
                // we're dropping the comment anyway it doesn't matter:
                // we stop at the first `\n` regardless and let the
                // outer body terminator (the bare `\n` at end-of-define)
                // handle the rest.
                while (i < raw_body.size() && raw_body[i] != '\n') ++i;
                if (i < raw_body.size()) {
                    // Preserve any whitespace structure: drop the
                    // comment text but keep the newline-equivalent as
                    // a single space (matches the LINE_CONT join above).
                    joined.push_back(' ');
                }
                continue;
            }
            joined.push_back(c);
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
    auto name = dict_string_or_empty(d, "name");
    if (!name.empty()) state_.macros.erase(name);
}

void Preprocessor::handle_include(const DictValue& d, std::string& out) {
    auto path = dict_string_or_empty(d, "path");
    if (path.empty()) {
        state_.warnings.push_back(
            {"`include: empty path", state_.current_file, state_.current_line});
        return;
    }

    std::string canonical;
    std::string include_text;

    // Host-supplied include source takes priority.
    if (opts_.include_source) {
        auto host = opts_.include_source(path, state_.current_file);
        if (host) {
            canonical    = std::move(host->canonical_id);
            include_text = std::move(host->content);
        }
    }

    // Built-in fallback: include_paths, then including file's dir, then
    // the path as given.
    if (canonical.empty() && include_text.empty()) {
        namespace fs = std::filesystem;
        fs::path resolved;
        bool found = false;
        fs::path requested = path;
        auto try_candidate = [&](const fs::path& base) {
            fs::path candidate = base.empty() ? requested : (base / requested);
            std::error_code ec;
            if (fs::exists(candidate, ec) && !ec) {
                resolved = candidate; found = true; return true;
            }
            return false;
        };
        for (const auto& dir : opts_.include_paths)
            if (try_candidate(dir)) break;
        if (!found && !state_.current_file.empty())
            try_candidate(fs::path(state_.current_file).parent_path());
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
        std::ostringstream buf; buf << f.rdbuf();
        include_text = buf.str();
    }

    bool already_seen = false;
    for (const auto& p : state_.included_files)
        if (p == canonical) { already_seen = true; break; }
    if (!already_seen) state_.included_files.push_back(canonical);

    auto saved_file = state_.current_file;
    auto saved_line = state_.current_line;
    state_.current_file = canonical;
    state_.current_line = 1;

    // Splice mode: recurse into the scan driver, appending the included
    // file's expansion to `out`. Side-effects-only mode still scans (so
    // macros / includes register) but discards the emitted bytes.
    if (opts_.splice) {
        scan_stream(include_text, out);
    } else {
        std::string discard;
        scan_stream(include_text, discard);
    }

    state_.current_file = std::move(saved_file);
    state_.current_line = saved_line;
}

std::string Preprocessor::expand_macro_use(const DictValue& d) {
    auto name = dict_string_or_empty(d, "name");
    if (name.empty()) return {};
    if (state_.macros.find(name) != state_.macros.end())
        return render_macro_use_inline(d);   // defined -> expand

    std::vector<std::string> args_text;
    if (auto av = dict_value_or_null(d, "args")) {
        if (auto arr = std::dynamic_pointer_cast<ArrayValue>(av))
            for (const auto& a : arr->data()) {
                if (auto s = std::dynamic_pointer_cast<StringValue>(a))
                    args_text.push_back(s->data());
                else
                    args_text.push_back(render_segment(a));
            }
    }
    // Undefined: host handler first, then the on_undefined policy.
    if (opts_.undefined_handler) {
        if (auto rep = opts_.undefined_handler(name, args_text)) {
            state_.active_expansions.insert(name);
            ++state_.current_depth;
            std::string e = expand_recursive(*rep);
            --state_.current_depth;
            state_.active_expansions.erase(name);
            return e;
        }
    }
    auto verbatim = [&]() {
        std::string s = "`" + name;
        if (!args_text.empty()) {
            s += "(";
            for (std::size_t i = 0; i < args_text.size(); ++i) {
                if (i) s += ",";
                s += args_text[i];
            }
            s += ")";
        }
        return s;
    };
    switch (opts_.on_undefined) {
        case PpOnUndefined::Leave: return verbatim();
        case PpOnUndefined::Empty: return {};
        case PpOnUndefined::Warn:
            state_.warnings.push_back(
                {"undefined macro `" + name, state_.current_file,
                 state_.current_line});
            return verbatim();
        case PpOnUndefined::Error:
            throw std::runtime_error("undefined macro `" + name);
    }
    return verbatim();
}

void Preprocessor::scan_stream(const std::string& text, std::string& out) {
    NodeId pc = pp_grammar_.rule_id("PP_CONSTRUCT");
    if (!pc.valid()) { out += text; return; }

    struct CondFrame {
        bool emitting;         // this branch active AND parent emitting
        bool any_taken;        // some branch in this group has matched
        bool parent_emitting;  // was output live when this group opened
        bool is_ifdef;         // ifdef-family: `elsif` tests definedness
    };
    std::vector<CondFrame> cstack;
    auto emitting = [&]() { return cstack.empty() || cstack.back().emitting; };

    auto stream = Stream::from_string(text);
    StreamReader& sr = stream.reader();
    ValuePool pool;
    const std::size_t n = text.size();

    auto eval_expr = [&](const ValuePtr& cond) -> bool {
        std::string s;
        if (auto sv = std::dynamic_pointer_cast<StringValue>(cond)) s = sv->data();
        trim_horiz(s);
        // A custom expr_eval receives the raw condition text; the built-in
        // eval_cond_default subparses it through COND_EXPR itself.
        ValuePtr c = make_string(s);
        ValuePtr v = opts_.expr_eval ? opts_.expr_eval(c) : eval_cond_default(c);
        if (v && v->type() == ValueType::Bool)
            return static_cast<const BoolValue*>(v.get())->data();
        switch (opts_.on_undecidable) {
        case PpOnUndecidable::TreatAsFalse:
            state_.warnings.push_back(
                {"`if condition undecidable; treating as false",
                 state_.current_file, state_.current_line});
            return false;
        case PpOnUndecidable::TreatAsTrue:
            state_.warnings.push_back(
                {"`if condition undecidable; treating as true",
                 state_.current_file, state_.current_line});
            return true;
        case PpOnUndecidable::Error:
            throw std::runtime_error("`if condition undecidable");
        }
        return false;
    };
    auto defined_ident = [&](const ValuePtr& cond) -> bool {
        std::string s;
        if (auto sv = std::dynamic_pointer_cast<StringValue>(cond)) s = sv->data();
        trim_horiz(s);
        std::size_t k = 0;
        while (k < s.size()
               && (std::isalnum((unsigned char)s[k]) || s[k] == '_')) ++k;
        s = s.substr(0, k);
        return !s.empty() && state_.macros.find(s) != state_.macros.end();
    };

    // Skip a stand-alone directive line's tail (trailing hspace + `//`
    // comment + newline) so it leaves no output; leave inline content.
    auto skip_line_tail = [&]() {
        while (!sr.eof()) {
            std::size_t i = sr.position().bytes;
            char c = text[i];
            if (c == ' ' || c == '\t' || c == '\r') { sr.get(); continue; }
            if (c == '/' && i + 1 < n && text[i + 1] == '/') {
                while (!sr.eof() && text[sr.position().bytes] != '\n') sr.get();
                continue;
            }
            break;
        }
        if (!sr.eof() && text[sr.position().bytes] == '\n') sr.get();
    };

    while (!sr.eof()) {
        std::size_t i = sr.position().bytes;
        char c = text[i];
        bool emit = emitting();

        if (c == '`') {
            sr.mark();
            auto r = pp_grammar_.parse_from(sr, pool, pc,
                                            /*require_full=*/false, nullptr);
            if (r && sr.position().bytes > i) {
                sr.accept();
                auto dd = std::dynamic_pointer_cast<DictValue>(*r);
                std::string type = dd ? dict_string_or_empty(*dd, "type") : "";
                if (type == "define") {
                    if (emit) handle_define(*dd);      // grammar ate the `\n`
                } else if (type == "undef") {
                    if (emit) handle_undef(*dd);
                    skip_line_tail();
                } else if (type == "include") {
                    if (emit) handle_include(*dd, out);
                    skip_line_tail();
                } else if (type == "ifdef" || type == "ifndef") {
                    std::string cond = dict_string_or_empty(*dd, "cond");
                    bool def = state_.macros.find(cond) != state_.macros.end();
                    bool raw = (type == "ifndef") ? !def : def;
                    cstack.push_back({emit && raw, raw, emit, true});
                    skip_line_tail();
                } else if (type == "pp_if") {
                    bool raw = eval_expr(dict_value_or_null(*dd, "cond"));
                    cstack.push_back({emit && raw, raw, emit, false}); // ate `\n`
                } else if (type == "elsif") {
                    if (!cstack.empty()) {
                        auto& f = cstack.back();
                        bool take = false;
                        if (f.parent_emitting && !f.any_taken) {
                            take = f.is_ifdef
                                ? defined_ident(dict_value_or_null(*dd, "cond"))
                                : eval_expr(dict_value_or_null(*dd, "cond"));
                        }
                        f.emitting = take;
                        if (take) f.any_taken = true;
                    }                                            // ate `\n`
                } else if (type == "else") {
                    if (!cstack.empty()) {
                        auto& f = cstack.back();
                        f.emitting = f.parent_emitting && !f.any_taken;
                        f.any_taken = true;
                    }
                    skip_line_tail();
                } else if (type == "endif") {
                    if (!cstack.empty()) cstack.pop_back();
                    skip_line_tail();
                } else if (type == "macro_use") {
                    if (emit) {
                        // LRM §22.5.1: a function-like macro may have
                        // whitespace before its `(args)`. PP_MACRO_USE is
                        // tight (it stops at `\`NAME`), so if NAME is
                        // function-like and `(` follows after linespace,
                        // lift those args into the call. Object-like uses
                        // leave the whitespace as pass-through content.
                        bool has_args = false;
                        if (auto av = dict_value_or_null(*dd, "args"))
                            if (auto aa =
                                std::dynamic_pointer_cast<ArrayValue>(av))
                                has_args = !aa->data().empty();
                        std::string nm = dict_string_or_empty(*dd, "name");
                        auto mit = state_.macros.find(nm);
                        bool fn_like = mit != state_.macros.end()
                                       && mit->second.is_function_like;
                        bool lifted = false;
                        if (!has_args && fn_like) {
                            std::size_t p = sr.position().bytes;
                            while (p < n && (text[p] == ' ' || text[p] == '\t'))
                                ++p;
                            std::vector<std::string> arglist;
                            std::size_t past = (p < n && text[p] == '(')
                                ? scan_args(text, p, arglist) : p;
                            if (past > p && !arglist.empty()) {
                                while (sr.position().bytes < past && !sr.eof())
                                    sr.get();
                                auto with = std::make_shared<DictValue>();
                                for (const auto& kv : dd->data())
                                    with->data()[kv.first] = kv.second;
                                auto aa = std::make_shared<ArrayValue>();
                                for (auto& a : arglist)
                                    aa->data().push_back(make_string(a));
                                with->data()["args"] = aa;
                                out += expand_macro_use(*with);
                                lifted = true;
                            }
                        }
                        if (!lifted) out += expand_macro_use(*dd);
                    }
                } else {
                    if (emit) out.append(text, i, sr.position().bytes - i);
                }
            } else {
                sr.reject();
                if (emit) out.push_back('`');
                sr.get();
            }
            continue;
        }

        if (c == '"') {                                     // string literal
            std::size_t start = i;
            sr.get();
            while (!sr.eof()) {
                char q = text[sr.position().bytes];
                sr.get();
                if (q == '\\' && !sr.eof()) { sr.get(); continue; }
                if (q == '"') break;
            }
            if (emit) out.append(text, start, sr.position().bytes - start);
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {   // line comment
            std::size_t start = i;
            while (!sr.eof() && text[sr.position().bytes] != '\n') sr.get();
            if (emit) out.append(text, start, sr.position().bytes - start);
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '*') {   // block comment
            std::size_t start = i;
            sr.get(); sr.get();
            while (!sr.eof()) {
                std::size_t k = sr.position().bytes;
                if (text[k] == '*' && k + 1 < n && text[k + 1] == '/') {
                    sr.get(); sr.get(); break;
                }
                sr.get();
            }
            if (emit) out.append(text, start, sr.position().bytes - start);
            continue;
        }

        sr.get();
        if (emit) out.push_back(c);
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
