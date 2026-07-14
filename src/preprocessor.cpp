#include <rawast/preprocessor.hpp>
#include <functional>

#include <rawast/builder.hpp>
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

std::string_view to_string(PpOnMissingInclude v) noexcept {
    switch (v) {
        case PpOnMissingInclude::Error: return "error";
        case PpOnMissingInclude::Warn:  return "warn";
        case PpOnMissingInclude::Leave: return "leave";
    }
    return "";
}

std::optional<PpOnMissingInclude> parse_pp_on_missing_include(
        std::string_view name) noexcept {
    if (name == "error") return PpOnMissingInclude::Error;
    if (name == "warn")  return PpOnMissingInclude::Warn;
    if (name == "leave") return PpOnMissingInclude::Leave;
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
    // Root provenance span: the whole input file. Source-structure only
    // (no output offset); emitted runs become its descendants so `stack_at`
    // can walk output → source.
    std::uint32_t root = record_span(
        Span::NoParent, /*parent_offset=*/0, text.size(), Span::NoOutput,
        state_.current_file.empty() ? "<input>" : state_.current_file);
    std::string out;
    scan_stream(text, out, root);
    return out;
}
// scan_stream / expand_macro_use are defined lower down, after the
// file-local expansion helpers (trim_horiz, render_segment, …) they use.

std::uint32_t Preprocessor::record_span(std::uint32_t parent_id,
                                       std::size_t parent_offset,
                                       std::size_t length,
                                       std::size_t out_offset,
                                       std::string name,
                                       bool collapse) {
    Span s;
    s.id = static_cast<std::uint32_t>(state_.spans.size());
    s.parent_id = parent_id;
    s.parent_offset = parent_offset;
    s.length = length;
    s.out_offset = out_offset;
    s.name = std::move(name);
    s.collapse = collapse;
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
        // The byte at `cur_offset` within `cur` maps into the parent.
        // Normally linearly (`parent_offset + cur_offset`); for a
        // collapse span (a macro expansion) every byte maps to the
        // single use-site position `parent_offset`, so an error deep
        // in the expansion points at the use site, not drifted past it.
        cur_offset = cur->collapse ? cur->parent_offset
                                   : cur->parent_offset + cur_offset;
        cur = &state_.spans[cur->parent_id];
    }
    return frames;
}

namespace {


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
//   - StringValue (a literal text run) → keep verbatim. Every param
//     reference is its own `ref` segment (above), so a text run holds
//     only operators / whitespace / punctuation — nothing to substitute.
//   - Other DictValues (macro_use, string literal, etc.) → keep
//     verbatim. render_macro_body_segments handles them — macro_use
//     recurses through render_macro_use_inline, so nested expansion
//     stays in AST land.
//
// `args` is positional; arity-mismatched calls pass an empty list
// from handle_macro_use to disable per-name substitution while
// still firing stringify/paste markers in text segments.
// A grammar segmenter: turns a raw text run (a macro-call argument)
// into typed body segments via the grammar's MACRO_BODY rule. Threaded
// into substitute_segments so a nested call's args — captured by
// MACRO_ARGS as RAW balanced text — get the SAME grammar-driven
// segmentation the body gets, instead of char-level text substitution.
// Provided by the Preprocessor (segment_body). May be empty (no
// segmenter) in unit contexts, in which case the raw-text fallback runs.
using Segmenter = std::function<std::shared_ptr<ArrayValue>(const std::string&)>;

// Render already-substituted arg segments back to a nested call's
// argument text. Strings and stringify are ATOMIC — a `type:"string"`
// segment renders its `"..."` verbatim, so macro formals are never
// substituted inside a string (that job belongs to segmentation, not a
// char-level string-skip). Macro uses render as `\`NAME(args)` text,
// unexpanded (they expand when the nested call does). Extends
// render_segment with stringify.
std::string render_arg_text(const ArrayValue& segs) {
    std::string out;
    for (const auto& seg : segs.data()) {
        if (!seg) continue;
        if (auto d = as_dict(seg)) {
            if (dict_string_or_empty(*d, "type") == "stringify") {
                std::string inner;
                if (auto s = as_array(dict_value_or_null(*d, "segments")))
                    inner = render_arg_text(*s);
                out += "\"" + stringify_escape(inner) + "\"";
                continue;
            }
        }
        out += render_segment(seg);
    }
    return out;
}

std::shared_ptr<ArrayValue> substitute_segments(
        const ArrayValue& body_segs,
        const std::vector<MacroParam>& params,
        const std::vector<ValuePtr>& args_in,
        const Segmenter& segment = {}) {
    // No whitespace normalisation of call-site args: the value is
    // whatever the grammar captured (MACRO_ARG). A trailing space it
    // kept renders as a cosmetic space downstream — semantically inert
    // (the SV lexer ignores it), and owning whitespace policy here would
    // be C++ doing the grammar's job.
    //
    // Emptiness is STRUCTURAL, not whitespace-derived: the grammar hands
    // an empty segment-list for an omitted arg (MACRO_ARGS `ignore
    // whitespace` consumes the inter-comma space before MACRO_ARG runs),
    // so `\`MAC(a, , c)` gives [] for the middle arg. An empty positional
    // arg whose formal has a `= default_text` falls back to the default
    // (LRM is ambiguous on intermediate empty args; VCS/Verilator use the
    // default — e.g. `\`DV_WAIT_TIMEOUT(, , msg)`).
    auto as_seglist = [](const ValuePtr& v) -> std::shared_ptr<ArrayValue> {
        if (auto arr = std::dynamic_pointer_cast<ArrayValue>(v)) return arr;
        auto one = std::make_shared<ArrayValue>();
        if (v) one->data().push_back(v);
        return one;
    };
    std::vector<ValuePtr> args;
    args.reserve(args_in.size());
    for (std::size_t i = 0; i < args_in.size(); ++i) {
        auto sl = as_seglist(args_in[i]);
        if (sl->data().empty() && i < params.size()
                && !params[i].default_text.empty()) {
            auto def = std::make_shared<ArrayValue>();
            def->data().push_back(make_string(params[i].default_text));
            args.push_back(def);
        } else {
            args.push_back(sl);
        }
    }

    std::unordered_map<std::string, std::size_t> param_idx;
    bool has_params = (params.size() == args.size()) && !params.empty();
    if (has_params) {
        for (std::size_t i = 0; i < params.size(); ++i) {
            param_idx[params[i].name] = i;
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
        if (std::dynamic_pointer_cast<StringValue>(seg)) {
            // A body text run is LITERAL. MACRO_BODY tokenises every
            // parameter reference into its own `ref` segment (spliced
            // below), so a StringValue here holds only operators /
            // whitespace / punctuation — never a bare param name. There
            // is nothing to substitute; pass it through untouched. (A
            // text-level find-replace here would also be *wrong*: it
            // would match `x` inside the identifier `x_and_axb`.)
            result->data().push_back(seg);
            continue;
        }
        if (auto d = std::dynamic_pointer_cast<DictValue>(seg)) {
            auto type = dict_string_or_empty(*d, "type");
            // `\`\`` token paste (IEEE 1800-2017 §22.5.1): the `` operator
            // means "drop me so the two neighbouring tokens become one". We
            // do NOT hand-fuse spellings or match call parens — the GRAMMAR
            // does both. Spell the left operand and everything to the right
            // of the `` with NOTHING between at each paste point (a `` spells
            // empty so its neighbours touch; a `ref` resolves to its bound
            // arg; a `\`"…`"` stringify to its quoted literal), then hand the
            // whole run back to segment_body — the same grammar we split
            // with. It fuses the now-contiguous tokens into one token AND
            // captures any following call `(args)` for free; the re-lexed
            // segments splice back and expand like any other. Chained pastes
            // (`a``b``c`) fall out for free: every `` spells empty, so the
            // operands are contiguous in the run.
            if (type == "token_paste") {
                if (result->data().empty() || si + 1 >= segdata.size())
                    continue;
                ValuePtr left = result->data().back();
                result->data().pop_back();
                // Left operand (already substituted — it came off `result`):
                // a macro_use spells as a bare `\`NAME` so it fuses into the
                // constructed name; anything else via render_segment.
                std::string run;
                if (auto ld = std::dynamic_pointer_cast<DictValue>(left);
                        ld && dict_string_or_empty(*ld, "type") == "macro_use")
                    run = "`" + dict_string_or_empty(*ld, "name");
                else
                    run = render_segment(left);
                // Substitute the REST (right operand onward) FIRST, via the
                // same walker — recursively, so refs nested inside a
                // following macro-call's args (`\`M(P)` where P is a param)
                // resolve too, which a shallow spell() would miss. Then
                // render it to text and append with NOTHING between at the
                // paste point.
                auto rest = std::make_shared<ArrayValue>();
                for (std::size_t j = si + 1; j < segdata.size(); ++j)
                    rest->data().push_back(segdata[j]);
                auto rest_sub =
                    substitute_segments(*rest, params, args, segment);
                for (const auto& rs : rest_sub->data())
                    run += render_segment(rs);
                si = segdata.size();
                // A multi-line macro arg renders with its inter-token
                // newline; but a macro expansion is ONE logical line and
                // segment_body's MACRO_BODY stops at `\n` (the define
                // terminator), which would truncate the re-lex. Fold the
                // newline to a space so the whole run tokenises.
                for (char& c : run)
                    if (c == '\n' || c == '\r') c = ' ';
                // Hand the whole contiguous run back to segment_body — the
                // same grammar we split with. It fuses the now-touching
                // tokens into one and captures any following call `(args)`
                // for free; the re-lexed segments splice back and expand at
                // render. (Bind to a NAMED local — iterating
                // `segment(run)->data()` directly dangles: the temporary
                // shared_ptr dies before the loop body.)
                if (segment) {
                    auto relexed = segment(run);
                    for (const auto& rs : relexed->data())
                        result->data().push_back(rs);
                } else {
                    result->data().push_back(make_string(std::move(run)));
                }
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
                    sub_inner = substitute_segments(*inner_segs, params, args,
                                                    segment);
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
            // Balanced-group arg segment ({type:"group_*", items:[…]}):
            // atomic for splitting, but its items ARE substitutable — so
            // recurse to resolve param refs inside `(…)`/`{…}`/`[…]`.
            if (has_params
                && (type == "group_paren" || type == "group_brace"
                    || type == "group_bracket")) {
                auto items = as_array(dict_value_or_null(*d, "items"));
                auto sub_items = items
                    ? substitute_segments(*items, params, args, segment)
                    : std::make_shared<ArrayValue>();
                auto ng = std::make_shared<DictValue>();
                for (const auto& [k, v] : d->data())
                    ng->data()[k] = (k == "items") ? sub_items : v;
                result->data().push_back(ng);
                continue;
            }
            // Nested macro_use: substitute the OUTER params into its arg
            // list before the inner call expands (LRM §22.5.1). Each
            // inner arg is a SEGMENT-LIST, so args stay AST here — no
            // re-parse, no render-to-text round-trip per nesting level.
            // A whole arg that is exactly `ref(PARAM)` passes the outer
            // arg's segment-list through; anything else recurses.
            if (type == "macro_use" && has_params) {
                auto inner_args = as_array(dict_value_or_null(*d, "args"));
                if (inner_args && !inner_args->data().empty()) {
                    auto new_args = std::make_shared<ArrayValue>();
                    for (const auto& a : inner_args->data()) {
                        auto arg_sl = as_array(a);
                        if (!arg_sl) { new_args->data().push_back(a); continue; }
                        if (arg_sl->data().size() == 1) {
                            if (auto rd = as_dict(arg_sl->data()[0])) {
                                if (dict_string_or_empty(*rd, "type") == "ref") {
                                    auto it = param_idx.find(
                                        dict_string_or_empty(*rd, "value"));
                                    if (it != param_idx.end()) {
                                        new_args->data().push_back(
                                            args[it->second]);
                                        continue;
                                    }
                                }
                            }
                        }
                        new_args->data().push_back(substitute_segments(
                            *arg_sl, params, args, segment));
                    }
                    auto new_use = std::make_shared<DictValue>();
                    for (const auto& [k, v] : d->data())
                        new_use->data()[k] = (k == "args") ? new_args : v;
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

namespace { std::string render_segment(const ValuePtr& seg); }

std::string MacroDef::body_text() const {
    // Grammar macros defer segmentation, so before first expansion
    // `body_segments` is null — the raw body IS the flat text. Once
    // segmented, render the bare text runs (typed segments skipped),
    // preserving the prior contract for callers that see a cached body.
    if (!body_segments) return body_raw;
    std::string out;
    for (const auto& seg : body_segments->data()) {
        out += render_segment(seg);
    }
    return out;
}

std::shared_ptr<ArrayValue> Preprocessor::segment_body(
        const std::string& raw) const {
    NodeId mb = pp_grammar_.rule_id("MACRO_BODY");
    if (!mb.valid()) return std::make_shared<ArrayValue>();
    // MACRO_BODY's scope stops at a `\n`; append one as the sentinel so
    // the stored (newline-free) body always terminates.
    auto stream = Stream::from_string(raw + "\n");
    StreamReader& sr = stream.reader();
    ValuePool pool;
    auto r = pp_grammar_.parse_from(sr, pool, mb, /*require_full=*/false,
                                    nullptr);
    if (r) {
        if (auto d = std::dynamic_pointer_cast<DictValue>(*r)) {
            if (auto it = d->data().find("segments"); it != d->data().end()) {
                if (auto arr = as_array(it->second)) return arr;
            }
        }
    }
    return std::make_shared<ArrayValue>();
}

const ArrayValue& Preprocessor::segments_of(const MacroDef& m) const {
    if (!m.body_segments) m.body_segments = segment_body(m.body_raw);
    return *m.body_segments;
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
            const std::string& txt = sv->data();
            // A run with no backtick is pure literal (the common case).
            // A run that DOES carry a `\`NAME` only arises when token
            // paste / stringify synthesised a new spelling into a body
            // text run (or a legacy string body). Re-tokenise it THROUGH
            // THE GRAMMAR (segment_body) and expand in the segment domain
            // via render_macro_use_inline — no C++ char-scan
            // (expand_recursive) and no arg-splitting (scan_args).
            if (txt.find('`') == std::string::npos) out += txt;
            else out += render_macro_body_segments(*segment_body(txt));
            continue;
        }
        if (auto d = std::dynamic_pointer_cast<DictValue>(seg)) {
            auto type = dict_string_or_empty(*d, "type");
            if (type == "macro_use") {
                // A `\`INC (Y)` (space before `(`) is captured WITH its args
                // by MACRO_BODY (MACRO_ARGS `?linespace`), so it arrives as a
                // normal macro_use here — no body-side lift, no re-scan.
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
            // Balanced-group arg segment — emit brackets and RECURSE so
            // macro uses inside the group expand (render_segment would
            // emit them literally).
            if (type == "group_paren" || type == "group_brace"
                || type == "group_bracket") {
                const char* o = type == "group_brace" ? "{"
                              : type == "group_bracket" ? "[" : "(";
                const char* c = type == "group_brace" ? "}"
                              : type == "group_bracket" ? "]" : ")";
                out += o;
                if (auto items = std::dynamic_pointer_cast<ArrayValue>(
                        dict_value_or_null(*d, "items")))
                    out += render_macro_body_segments(*items);
                out += c;
                continue;
            }
            // ref / string / other typed segments — render leaf.
            out += render_segment(seg);
        }
    }

    // No post-render `//` strip: comments are discarded in the grammar
    // — DEFINE_BODY (bodies) and MACRO_ARG / ARG_ITEMS_* (args, incl.
    // inside groups) all carry `ignore line_comment[_cont] block_comment`,
    // so no comment ever reaches the rendered output.
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

    // Depth bound — blue-paint stops cycles, but a deep chain of DISTINCT
    // macros could still recurse without limit; cap it.
    if (state_.current_depth >= opts_.max_expansion_depth) {
        state_.warnings.push_back(
            {"preprocessor: max_expansion_depth (" +
             std::to_string(opts_.max_expansion_depth) +
             ") reached; emitting verbatim",
             state_.current_file, state_.current_line});
        return verbatim();
    }

    auto it = state_.macros.find(name);
    if (it == state_.macros.end()) {
        // Undefined nested use — leave verbatim. The on_undefined
        // policy fires at the source-mapped handle_macro_use entry,
        // not for synthesised inner uses arising from expansion.
        return verbatim();
    }
    const auto& macro = it->second;
    auto seg = [this](const std::string& s) { return segment_body(s); };

    // Object-like macros take NO arguments: a `(...)` after the name is
    // following TEXT, not an argument list (the grammar captured it as
    // args syntactically, but only the table knows the macro is
    // object-like — LRM §22.5.1). Expand the body, then re-emit the
    // parens as text: `\`W(3)` with `\`define W 8` → `8(3)`, not `8`.
    if (!macro.is_function_like && !args.empty()) {
        state_.active_expansions.insert(name);
        ++state_.current_depth;
        std::string body = render_macro_body_segments(
            *substitute_segments(segments_of(macro), {}, {}, seg));
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
        substituted = substitute_segments(segments_of(macro), {}, {}, seg);
    } else {
        substituted = substitute_segments(segments_of(macro),
                                          macro.params, args_filled, seg);
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

std::string render_arg_text(const ArrayValue& segs);   // fwd (defined earlier)

std::string render_macro_args(const ArrayValue& args) {
    std::string out = "(";
    for (std::size_t i = 0; i < args.data().size(); ++i) {
        if (i > 0) out += ',';
        // Each arg is a segment-list (MACRO_ARG scope array); render it
        // as text. StringValue fallback for any legacy/synth arg.
        if (auto arr = as_array(args.data()[i])) out += render_arg_text(*arr);
        else if (auto s = as_string(args.data()[i])) out += s->data();
    }
    out += ')';
    return out;
}

std::string render_segment(const ValuePtr& seg) {
    if (!seg) return {};
    // A macro arg is a segment-list (ArrayValue); render it as text.
    if (auto arr = as_array(seg)) return render_arg_text(*arr);
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
    } else if (type == "group_paren" || type == "group_brace"
               || type == "group_bracket") {
        // Balanced-group arg segment — emit brackets around its items.
        const char* open  = type == "group_brace" ? "{"
                          : type == "group_bracket" ? "[" : "(";
        const char* close = type == "group_brace" ? "}"
                          : type == "group_bracket" ? "]" : ")";
        std::string out = open;
        if (auto v = d->data().find("items"); v != d->data().end())
            if (auto arr = as_array(v->second)) out += render_arg_text(*arr);
        out += close;
        return out;
    } else if (type == "stringify") {
        std::string inner;
        if (auto v = d->data().find("segments"); v != d->data().end())
            if (auto arr = as_array(v->second)) inner = render_arg_text(*arr);
        return "\"" + stringify_escape(inner) + "\"";
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
            // DEFINE_BODY discards `//` and `/* */` comments in the grammar
            // and captures strings atomically (<STRING> inner), so the body
            // segments are: text runs, null LINE_CONT boundaries, and
            // {type:"string"} dicts — NO comments. Join into the raw body:
            // text verbatim (LINE_CONT already consumed the `\<newline>`s;
            // nulls drop = continuation fusion), string segments re-quoted.
            std::string body_raw;
            for (const auto& seg : body_arr->data()) {
                if (!seg) continue;                     // LINE_CONT boundary
                if (auto vs = as_string(seg)) {
                    body_raw += vs->data();
                } else if (auto sd = as_dict(seg)) {
                    if (dict_string_or_empty(*sd, "type") == "string")
                        body_raw += "\"" + dict_string_or_empty(*sd, "value")
                                    + "\"";
                }
            }
            m.body_raw = std::move(body_raw);
            state_.macros[m.name] = std::move(m);
            return;
        }
    }

    // Fallback for a grammar that captures `body` as a plain STRING instead
    // of a segmented DEFINE_BODY array. The MACRO_BODY contract expects
    // segmentation — that is how parameter refs, comments, and line
    // continuations are resolved — so an unsegmented body is stored as one
    // opaque object-like literal. No C++ char-tokenising (no `//`-strip, no
    // split_params, no trim). No conforming grammar reaches here (SV +
    // backtick_pp both segment bodies).
    m.body_segments = std::make_shared<ArrayValue>();
    m.body_segments->data().push_back(
        make_string(dict_string_or_empty(d, "body")));
    m.is_function_like = false;
    state_.macros[m.name] = std::move(m);
}

void Preprocessor::handle_undef(const DictValue& d) {
    auto name = dict_string_or_empty(d, "name");
    if (!name.empty()) state_.macros.erase(name);
}

void Preprocessor::handle_include(const DictValue& d, std::string& out,
                                  std::uint32_t parent_span,
                                  std::size_t use_offset) {
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
            // A missing include means incomplete output. Default Error is a
            // hard failure (like verilator); lenient callers pick Warn/Leave.
            switch (opts_.on_missing_include) {
                case PpOnMissingInclude::Error:
                    throw std::runtime_error(
                        "`include: file not found: '" + path + "'");
                case PpOnMissingInclude::Warn:
                    state_.warnings.push_back(
                        {"`include: file not found: '" + path + "'",
                         state_.current_file, state_.current_line});
                    return;
                case PpOnMissingInclude::Leave:
                    return;
            }
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

    // Child provenance span for the included file, parented to the
    // `\`include` site in the including file. The recursive scan attaches
    // the included file's runs beneath it, so an error inside the include
    // backtraces: run → included file → `\`include` site → … → root.
    std::uint32_t include_span = record_span(
        parent_span, /*parent_offset=*/use_offset, include_text.size(),
        Span::NoOutput, canonical);

    // Splice mode: recurse into the scan driver, appending the included
    // file's expansion to `out`. Side-effects-only mode still scans (so
    // macros / includes register) but discards the emitted bytes.
    if (opts_.splice) {
        scan_stream(include_text, out, include_span);
    } else {
        std::string discard;
        scan_stream(include_text, discard, include_span);
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
            // The handler returned replacement TEXT — re-tokenise it
            // through the grammar and expand in the segment domain.
            std::string e = render_macro_body_segments(*segment_body(*rep));
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

void Preprocessor::scan_stream(const std::string& text, std::string& out,
                               std::uint32_t parent_span) {
    NodeId pc = pp_grammar_.rule_id("PP_CONSTRUCT");
    if (!pc.valid()) { out += text; return; }

    // Predefined `\`__LINE__` / `\`__FILE__` + `\`line` renumbering support.
    // current_line isn't tracked per-line during the scan, so compute the
    // physical line (1 + newlines before the offset) on demand; a `\`line N`
    // sets line_offset so the line AFTER the directive reads as logical N
    // (logical = physical + line_offset).
    long line_offset = 0;
    auto physical_line = [&](std::size_t off) -> long {
        long p = 1;
        const std::size_t n = std::min(off, text.size());
        for (std::size_t k = 0; k < n; ++k) if (text[k] == '\n') ++p;
        return p;
    };
    // For opts_.emit_line_directives: parser_line is the line number a
    // `\`line`-aware downstream consumer would compute at the current output
    // position. When a new OUTPUT line starts at a source line that differs
    // (a directive was stripped, a multi-line call collapsed, a dead branch
    // was skipped, an include boundary crossed), emit a `\`line` marker so
    // the consumer re-syncs — the standard C/verilator way to keep source
    // line numbers meaningful after preprocessing.
    long parser_line = 1;

    // Emit a pass-through run [src_off, src_off+len) and record a mapping
    // span: its output range maps linearly back to source, and its parent
    // chain names the file (and any including file). Named "text" so the
    // named file/include spans above it are distinguishable in stack_at.
    auto emit_text = [&](std::size_t src_off, std::size_t len) {
        if (!len) return;
        if (opts_.emit_linenum_prefix.empty()) {
            record_span(parent_span, src_off, len, out.size(), "text");
            out.append(text, src_off, len);
            return;
        }
        // Line-directive mode: walk the run one output line at a time. At
        // each output-line START, if the true source line differs from what
        // a directive-aware consumer would compute (parser_line) — which
        // drifts whenever a directive/branch was stripped or a multi-line
        // call collapsed — emit a `<prefix> N "file" 0` marker to re-sync.
        // Each output newline advances parser_line by one.
        const std::size_t end = src_off + len;
        std::size_t pos = src_off;
        while (pos < end) {
            if (out.empty() || out.back() == '\n') {
                const long src = physical_line(pos) + line_offset;
                if (src != parser_line) {
                    out += opts_.emit_linenum_prefix + " " +
                           std::to_string(src) + " \"" +
                           (state_.current_file.empty()
                                ? std::string("<input>") : state_.current_file)
                           + "\" 0\n";
                    parser_line = src;   // the marker declares the NEXT line
                }
            }
            std::size_t nl = text.find('\n', pos);
            std::size_t chunk_end = (nl == std::string::npos || nl >= end)
                                    ? end : nl + 1;
            record_span(parent_span, pos, chunk_end - pos, out.size(), "text");
            out.append(text, pos, chunk_end - pos);
            if (text[chunk_end - 1] == '\n') ++parser_line;
            pos = chunk_end;
        }
    };
    // Emit a macro expansion. Its bytes come from the macro body, not the
    // source at the use site, so the whole run is attributed to the use
    // site (`use_off`) — "error in expansion of `\`NAME` used here", the
    // standard preprocessor diagnostic.
    auto emit_expansion = [&](const std::string& exp, std::size_t use_off) {
        if (exp.empty()) return;
        record_span(parent_span, use_off, exp.size(), out.size(),
                    "macro expansion", /*collapse=*/true);
        out += exp;
    };

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

    auto eval_expr = [&](const ValuePtr& cond) -> bool {
        std::string s;
        if (auto sv = std::dynamic_pointer_cast<StringValue>(cond)) s = sv->data();
        // cond arrives already boundary-clean: PP_IF/PP_ELSIF consume the
        // leading whitespace in the grammar (`?linespace` before the raw
        // `*:cond=@`), so neither the built-in COND_EXPR subparse nor a
        // user-supplied expr_eval sees a leading space.
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
        auto sv = std::dynamic_pointer_cast<StringValue>(cond);
        if (!sv) return false;
        // `\`elsif NAME` in an `\`ifdef` chain names a macro. The cond is
        // captured RAW (`\`elsif` is dual-purpose — an expression in an
        // `\`if` chain, a macro name here), so extract the identifier the
        // GRAMMAR's way: lazily subparse the raw text through PP_MACRO_NAME
        // ON DEMAND — only in this ifdef-chain branch, never eagerly at parse
        // time. Mirrors eval_cond_default's PP_COND subparse; no hardcoded
        // identifier charset in the driver.
        auto stream = Stream::from_string(sv->data());
        auto r = pp_grammar_.parse_from(stream, "PP_MACRO_NAME");
        if (!r) return false;
        auto dd = std::dynamic_pointer_cast<DictValue>(*r);
        std::string name = dd ? dict_string_or_empty(*dd, "name") : "";
        return !name.empty() && state_.macros.find(name) != state_.macros.end();
    };


    // One builder, reused for every per-byte probe (reset() between).
    // parse_into feeds this builder instead of parse_from constructing a
    // fresh SharedPtrBuilder per byte — the per-probe allocation the profile
    // flagged as dominant on the scan hot path.
    SharedPtrBuilder scan_builder(pool);
    // Buffer the plain-text run between tokens: an unmatched byte extends a
    // pending run instead of emitting a per-byte span (record_span per byte
    // was the last per-byte allocation). The run is flushed as ONE span when
    // a token matches or at EOF — byte-identical output, one span per run
    // (what the old batched loop produced). `emit` is constant within a run:
    // it only changes at a directive, which is a match, which flushes first.
    std::size_t pending_start = SIZE_MAX;
    bool pending_emit = false;
    auto flush_text = [&](std::size_t end) {
        if (pending_start == SIZE_MAX) return;
        if (pending_emit) emit_text(pending_start, end - pending_start);
        pending_start = SIZE_MAX;
    };
    while (!sr.eof()) {
        const std::size_t i = sr.position().bytes;
        const bool emit = emitting();

        // Fully grammar-driven scan: try PP_CONSTRUCT at the cursor. Its
        // alternatives — directive / macro use / string / comment — each
        // begin with the sigil, `"`, or `/`, so a byte that starts no token
        // fails on the very first literal compare. "Try the parse here" is
        // therefore equivalent to the lexical check the driver used to
        // hardcode (`if (c == '`')` plus the string/comment skips), but now
        // EVERY lexical fact — the sigil AND the string/comment delimiters —
        // lives in the grammar. The driver acts only on directives; every
        // other matched token (string, comment) and every unmatched byte is
        // copied through verbatim (the raw consumed span, byte-exact).
        scan_builder.reset();
        sr.mark();
        auto r = pp_grammar_.parse_into(sr, pool, pc, scan_builder,
                                        /*require_full=*/false, nullptr);
        if (r && sr.position().bytes > i) {
            sr.accept();
            flush_text(i);   // the plain-text run ended where this token began
            auto dd = std::dynamic_pointer_cast<DictValue>(scan_builder.result());
            std::string type = dd ? dict_string_or_empty(*dd, "type") : "";
            // Every directive rule consumes its own line-tail (trailing
            // space + `//` comment + newline) in the grammar, so the driver
            // no longer skips it.
            if (type == "define") {
                if (emit) handle_define(*dd);      // grammar ate the `\n`
            } else if (type == "undef") {
                if (emit) handle_undef(*dd);
            } else if (type == "include") {
                if (emit) handle_include(*dd, out, parent_span, i);
            } else if (type == "ifdef" || type == "ifndef") {
                std::string cond = dict_string_or_empty(*dd, "cond");
                bool def = state_.macros.find(cond) != state_.macros.end();
                bool raw = (type == "ifndef") ? !def : def;
                cstack.push_back({emit && raw, raw, emit, true});
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
            } else if (type == "endif") {
                if (!cstack.empty()) cstack.pop_back();
            } else if (type == "undefineall") {
                if (emit) state_.macros.clear();          // §22.5.2
            } else if (type == "line") {
                // `\`line N "file" level`: the NEXT physical line becomes
                // logical N, so line_offset = N - (this line) - 1.
                if (emit) {
                    long n = 0;
                    if (auto lv = dict_value_or_null(*dd, "line")) {
                        if (auto iv = std::dynamic_pointer_cast<IntValue>(lv))
                            n = static_cast<long>(iv->data());
                        else if (auto s = std::dynamic_pointer_cast<StringValue>(lv))
                            { try { n = std::stol(s->data()); } catch (...) {} }
                    }
                    line_offset = n - physical_line(i) - 1;
                    std::string f = dict_string_or_empty(*dd, "file");
                    if (!f.empty()) state_.current_file = f;
                }
            } else if (type == "macro_use") {
                // Predefined `\`__LINE__`/`\`__FILE__` (IEEE 1800-2017 §22.10)
                // are ordinary macros, refreshed to THIS use site right
                // before expanding. Because they live in the macro table like
                // any macro, the normal expansion resolves them everywhere —
                // top level AND nested inside an expanded body — with no
                // special-casing. Updated only at this top-level dispatch, so
                // a body's `\`__LINE__ reports where the ENCLOSING macro was
                // used, not where it was defined.
                auto set_predef = [&](const char* nm, std::string body) {
                    MacroDef m;
                    m.name = nm;
                    m.body_raw = std::move(body);
                    m.is_function_like = false;
                    state_.macros[nm] = std::move(m);
                };
                // Use the END of the consumed macro call, not its start: for
                // a multi-line invocation `\`M(a,\n b)`, `\`__LINE__` resolves
                // to the line where the call COMPLETES (matches verilator -E).
                set_predef("__LINE__", std::to_string(
                    physical_line(sr.position().bytes) + line_offset));
                set_predef("__FILE__", "\"" + (state_.current_file.empty()
                    ? std::string("<input>") : state_.current_file) + "\"");
                // The grammar captures `\`NAME`, and `(args)` after optional
                // linespace (MACRO_ARGS `?linespace`), so a space-before-paren
                // call like `\`INC (5)` already arrives with its args. Whether
                // those args are USED (function-like) or the parens re-emitted
                // as text (object-like) is the table-dependent DECISION, made
                // in render_macro_use_inline — the grammar just captures.
                if (emit) emit_expansion(expand_macro_use(*dd), i);
            } else {
                // string / comment / passthrough directive / any
                // non-acted-on token → emit the raw span verbatim. This is
                // where type="passthrough" (`\`timescale`, `\`pragma`, …)
                // lands, so compiler directives survive REGARDLESS of the
                // on_undefined policy (they never reach the macro path).
                if (emit) emit_text(i, sr.position().bytes - i);
            }
            continue;
        }
        sr.reject();
        // No token here — extend the pending plain-text run by one byte.
        if (pending_start == SIZE_MAX) { pending_start = i; pending_emit = emit; }
        sr.get();
    }
    flush_text(sr.position().bytes);   // trailing run at EOF
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
