#pragma once

#include <rawast/node.hpp>
#include <rawast/parser.hpp>
#include <rawast/stream.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rawast {

// String-form name for a PpRole enumerator. Matches the lowercase
// identifier used in `:#role="..."` grammar bindings. PpRole::None
// returns an empty string (it has no surface form). Total — covers
// every enumerator.
std::string_view to_string(PpRole role) noexcept;

// Parse a string into a PpRole. Accepts the lowercase identifiers
// listed in node.hpp's PpRole declaration. Returns std::nullopt for
// any unknown name; the loader maps that to a clear user error.
std::optional<PpRole> parse_pp_role(std::string_view name) noexcept;

// Policy for an undefined macro use. The default (Leave) emits the
// directive verbatim — the host parser then handles the token however
// it sees fit, which matches typical SystemVerilog behavior where
// undefined ``MACRO is left for downstream tools. The other values
// let users tighten or loosen the contract per-file.
enum class PpOnUndefined {
    Leave,   // emit `MACRO_NAME verbatim — default
    Error,   // stop processing with a PpError
    Warn,    // emit a warning and fall back to Leave behavior
    Empty,   // silently drop; expand to empty string
};

std::string_view to_string(PpOnUndefined v) noexcept;
std::optional<PpOnUndefined> parse_pp_on_undefined(std::string_view name) noexcept;

// Policy for an UNDECIDABLE `\`if` / `\`elsif` condition — one whose
// `expr_eval` evaluated to `Undefined` (e.g. it references a macro the
// host can't resolve). Distinct from PpOnUndefined, which is about
// undefined macro *uses*. Default reproduces the historical behavior:
// an undecidable branch is simply not taken.
enum class PpOnUndecidable {
    TreatAsFalse,  // branch not taken (default; warns)
    TreatAsTrue,   // branch taken (warns)
    Error,         // stop processing with a PpError
};

std::string_view to_string(PpOnUndecidable v) noexcept;
std::optional<PpOnUndecidable> parse_pp_on_undecidable(std::string_view name) noexcept;

// One macro definition. Function-like macros carry a non-empty
// `params` list AND `is_function_like = true`; an empty `params`
// list with `is_function_like = false` is the object-like form.
//
// `body_segments` keeps the body as an AST array — the same shape
// the grammar produces for `\`define`'s `body` field (mixed bare
// StringValue text runs interleaved with typed segments for
// PARAM_REF / STRING / macro_use). Substitution at expansion time
// is an AST-to-AST splice: walk the segments, replace each
// `{type:"ref"}` whose value matches a parameter name with the
// corresponding arg's value, then walk the result through the
// normal segment dispatch — no string round-trip, no re-parse.
//
// Legacy bodies that arrived as a single string (mini_preprocessor
// synthesized ASTs, process_ast callers) are normalised to a one-
// element segments array at register time.
// A single formal parameter on a function-like `\`define`.
// `default_text` is the raw substitution text (per IEEE 1800-2017
// §22.5.1) inserted when the call site omits the corresponding
// positional argument. Empty `default_text` means "no default".
struct MacroParam {
    std::string name;
    std::string default_text;
};

struct MacroDef {
    std::string name;
    std::vector<MacroParam> params;
    std::shared_ptr<ArrayValue> body_segments;
    bool is_function_like = false;

    // Convenience: text representation of the body's flat text
    // segments. For legacy bodies that wrap a single StringValue
    // this is the original captured text; for systemverilog.rawast
    // bodies with typed segments, this only renders the bare text
    // runs (typed segments like `{type:"ref"}` are skipped). Used
    // by tests and the default expr-eval ref resolver — not by the
    // expansion path, which walks segments directly.
    std::string body_text() const;
};

// A preprocessor warning surfaced at process time. Accumulated on
// the Preprocessor (and exposed via its read-only `warnings`
// property) so callers can post-mortem the run instead of relying
// on stderr.
struct PpWarning {
    std::string message;
    std::string file;   // empty if not file-scoped
    int         line = 0;
};

// One node in the source-provenance graph. Spans form a parent-
// linked tree (DAG if a macro body is expanded from multiple call
// sites, but treated as a tree per usage). Every byte in the
// preprocessed output traces back through a chain of Spans to its
// originating root.
//
// Spans serve two roles:
//
//   1. Output spans (out_offset != NoOutput) cover ranges of the
//      preprocessed output stream. The walker emits these as it
//      processes input.
//
//   2. Source-structure spans (out_offset == NoOutput) describe
//      where input bytes live (root input file, included files,
//      macro definition bodies). Other spans reference them as
//      parents.
//
// stack_at(out_offset) finds the leaf Span covering that offset
// and walks the parent chain to build a full source-frame list.
struct Span {
    static constexpr std::uint32_t NoParent = ~static_cast<std::uint32_t>(0);
    static constexpr std::size_t   NoOutput = ~static_cast<std::size_t>(0);

    std::uint32_t id            = 0;
    std::uint32_t parent_id     = NoParent;
    std::size_t   parent_offset = 0;        // byte offset within parent
    std::size_t   length        = 0;
    std::size_t   out_offset    = NoOutput; // byte in preprocessed output (or NoOutput)
    std::string   name;                     // "original.sv", "macro UVM_INFO body", etc.
};

// One layer in a source-provenance stack. `where` is the span's
// `name` field, `offset` is the byte offset within that span. The
// leaf frame is the immediate source of the byte queried; the
// root frame is the ultimate origin (typically an input file).
struct SourceFrame {
    std::string where;
    std::size_t offset = 0;
};

// Mutable state accumulated across one or more preprocessor runs.
// Owned by the Preprocessor class. Inspectable through the
// Preprocessor's accessor methods so tooling can post-mortem the
// run, snapshot/restore between files, and feed dynamic_macros /
// undefined_handler callbacks the same view the engine sees.
struct PreprocessorState {
    // Active macro table — populated by `define`, cleared by `undef`.
    std::unordered_map<std::string, MacroDef> macros;

    // Conditional-compilation stack. One entry per nested
    // `ifdef`/`ifndef`/`if`. True means the current branch is being
    // emitted; false means the current branch is suppressed. Walker
    // pushes on the directive, pops on `endif`.
    std::vector<bool> ifdef_stack;

    // Blue-painting set — names currently being expanded in the
    // active expansion chain. Prevents infinite recursion when a
    // macro body references itself or participates in a cycle.
    // Cleared as expansions unwind.
    std::unordered_set<std::string> active_expansions;

    // Files this Preprocessor has processed through `process_file`
    // or as a side-effect of `\`include` resolution. Order is the
    // order of first encounter; duplicates are suppressed (include
    // guards / include-once behavior is implicit).
    std::vector<std::string> included_files;

    // Warnings surfaced during processing. Cleared by `reset()`.
    std::vector<PpWarning> warnings;

    // Current recursion depth — bumped on macro expansion entry,
    // decremented on exit. Compared against
    // Preprocessor::max_expansion_depth; over-limit aborts with a
    // PpError.
    int current_depth = 0;

    // The file currently being processed. Empty for inline
    // `process(string)` calls. Carried through to `dynamic_macros`
    // callbacks as the `__FILE__` value source and used for
    // PpWarning attribution.
    std::string current_file;

    // 1-based line within `current_file` (or the inline string).
    // Walker bumps it as it crosses newlines in the text passed
    // through and through `\`include` re-entries.
    int current_line = 1;

    // Source-provenance graph. spans[0] is reserved for the active
    // root span (created at the start of each process / process_file
    // call); subsequent entries are children. Lookup via
    // Preprocessor::stack_at walks parent_id from a leaf up.
    std::vector<Span> spans;
};

// Forward declaration so Preprocessor can hold a Grammar reference
// without dragging the full grammar.hpp into every translation unit
// that includes preprocessor.hpp.
class Grammar;

// Result of a host-supplied include-source resolver. The
// `canonical_id` uniquely identifies the source (used for source-map
// provenance, `included_files()` dedup, and diagnostic labels) and
// `content` is the raw bytes to parse + walk.
struct PpIncludeSource {
    std::string canonical_id;
    std::string content;
};

// Construction options for Preprocessor. Top-level so the in-class
// default initializers don't need to be visible at the Preprocessor
// constructor's default-argument-deduction site (a nested struct
// with in-class member initializers and a `Options opts = {}`
// default argument trips a "default initializer needed inside
// enclosing class" diagnostic on Clang/GCC).
struct PpOptions {
    // Text processed before any user input. Each call to
    // `process` / `process_file` starts from the macro table
    // this leaves behind. Suitable for `-D NAME=VALUE` CLI
    // flags (translated to `\`define NAME VALUE` lines),
    // `-include FILE` (translated to `\`include "FILE"`), and
    // similar pre-run setup.
    std::string predefined;

    // Filesystem search path for `\`include`. First match wins.
    std::vector<std::string> include_paths;

    // `\`include` policy. False (default) processes the included
    // file for side effects only — macro state updates apply but
    // no text is spliced into the output stream. True splices
    // the processed text in C/C++ style.
    bool splice = false;

    // What to do when a macro use names an undefined macro.
    // Default Leave: emit the source verbatim (`\`MACRO_NAME`),
    // letting the host parser handle the token.
    PpOnUndefined on_undefined = PpOnUndefined::Leave;

    // Policy applied when a `\`if` / `\`elsif` condition is undecidable
    // (expr_eval returned `Undefined`). Default: treat as not-taken.
    PpOnUndecidable on_undecidable = PpOnUndecidable::TreatAsFalse;

    // Host-supplied evaluator for `\`if` / `\`elsif` expressions.
    // Called with the AST node the grammar produced for the
    // expression — typically a structured dict tree (e.g.
    // {type:"binop", op:"&&", lhs:..., rhs:...}), but a leaf
    // StringValue is fine for trivial cases like `\`if FOO` where
    // the grammar just captures the identifier text.
    //
    // Returns:
    //   true  — branch is taken
    //   false — branch is not taken; walker tries the next elsif or
    //           falls to else_branch
    //   nullopt — evaluator can't decide; walker records a warning
    //           and treats the branch as false (consumers can elevate
    //           to error via their own policy if they need to).
    //
    // If unset and the walker hits an `\`if` directive, no branch is
    // taken and a warning is recorded — `\`if` without an evaluator
    // is a configuration error.
    //
    // Passing the AST (not the raw source text) keeps the grammar
    // as the only parser in the system — the host walks the tree
    // the same way the preprocessor walker does, rather than
    // re-parsing the expression by hand.
    // Returns a ValuePtr: a Bool value (true/false) when the condition is
    // decidable, or `Undefined` (undefined_value()) when it can't be
    // decided — the engine then applies `on_undecidable`. A null return is
    // treated the same as Undefined.
    std::function<ValuePtr(const ValuePtr& cond)> expr_eval;

    // Host-supplied source resolver for `\`include`. When set, called
    // instead of (well, before — see fallback) the built-in
    // `include_paths` filesystem walk. Returns the canonical id and
    // text content of the included source, or nullopt to mean "not
    // found, fall back to the built-in walk."
    //
    // The `including_file` argument is the canonical id of the file
    // currently doing the `\`include` (empty for top-level / stdin),
    // letting hosts implement "resolve relative to the including
    // file" or any other context-sensitive policy.
    //
    // The `canonical_id` returned identifies the source for source-
    // map provenance, the `included_files()` dedup query, and
    // diagnostic file labels. Two `\`include` directives that
    // legitimately reference the same logical source should produce
    // the same canonical_id; the host decides the identity policy
    // (absolute path, virtual URI, content hash, …).
    //
    // The `content` is the raw bytes to be parsed and walked. The
    // host is free to read from disk, cache in memory, fetch over
    // the network, or generate on the fly — the preprocessor doesn't
    // care where the bytes come from.
    //
    // Multi-include with redefined macros works as expected: the
    // callback fires once per `\`include` directive, each call
    // processes the returned content with the macro table as it
    // stood at the call site. Hosts that cache content do not skip
    // processing — only the I/O.
    std::function<std::optional<PpIncludeSource>(
        const std::string& requested,
        const std::string& including_file)> include_source;

    // Host-supplied fallback for undefined macro uses. Called when
    // a `\`NAME` site references a macro not in the active table.
    // If it returns a value, that string is emitted as the expansion
    // (recursively re-expanded — host can return text containing
    // further `\`uses) and the `on_undefined` policy is bypassed.
    // If it returns nullopt, processing falls through to the
    // `on_undefined` policy as if the callback weren't set.
    //
    // Args mirror what the AST reports: macro name (without the
    // leading backtick) and the captured argument strings.
    // Default-empty means "no callback" — bypass; existing behaviour
    // is unchanged for callers that don't set it.
    std::function<std::optional<std::string>(
        const std::string& name,
        const std::vector<std::string>& args)> undefined_handler;

    // Macro expansion recursion limit. Protects against cycles
    // that blue-painting alone doesn't catch (e.g. mutual
    // expansion via include + redefine). Hitting the limit
    // aborts processing with a PpError.
    int max_expansion_depth = 200;

    // Capture per-directive events into trace storage. Useful for
    // tooling and debugging; default off keeps the hot path cheap.
    bool trace = false;
};

// Generic AST evaluator for preprocessor `\`if` conditions. Walks
// the documented expression-AST shape (the `\`if-cond rules in
// grammars/systemverilog.rawast; SV's COND_EXPR variant uses `name`,
// `integer`, and `func_call`, which the evaluator also accepts)
// and returns a ValuePtr tri-state:
//
//   true_value()      — condition holds
//   false_value()     — condition does not hold
//   undefined_value() — undecidable (unknown call name, non-int operand
//            of an arithmetic op, ref without a resolver). The engine
//            then applies its `on_undecidable` policy.
//
// The AST shape (uniform across any grammar emitting it):
//
//   {type:"int",    value: <int>}
//   {type:"ref",    value: <name>}
//   {type:"paren",  value: <expr>}
//   {type:"call",   name: <fn>, args: [<expr>, ...]}
//                               // built-in: name == "defined" with
//                               // a single ref arg returns the macro's
//                               // is_defined state. Any other name is
//                               // nullopt (host extension point).
//   {op: "&&"/"||"/"!"/"=="/"!="/"<"/">"/"<="/">="/"+"/"-"/"*"/"/"/"%",
//    args: [<expr>, ...]}        // operators evaluated as documented
//                               // in grammars/systemverilog.rawast
//
// `ref_resolver`: callback that returns the macro body for a name,
// or nullopt if not defined. Decouples the evaluator from any specific
// Preprocessor — testable in isolation, reusable by any host. For an
// integer-valued macro (`\`define WIDTH 32`), the resolver returns the
// body verbatim; the evaluator parses it as an integer when used in
// arithmetic context, otherwise the truthiness of "defined" alone
// drives boolean context (`\`if FOO` is true iff FOO is defined and
// resolves to a non-zero integer or to a string that doesn't parse as
// an integer — defined-and-non-empty is truthy).
ValuePtr default_pp_expr_eval(
    const ValuePtr& cond,
    const std::function<std::optional<std::string>(
        const std::string& name)>& ref_resolver);

// The user-facing entry point for preprocessing. Owns the active
// PreprocessorState; orchestrates parse + walk for the configured
// preprocessor grammar (e.g. `systemverilog`, entered at PP_FILE).
//
// Lifetimes: the Grammar reference must outlive the Preprocessor.
// Typical pattern is to construct the Preprocessor with a Grammar
// loaded by the caller, reuse the Preprocessor across many
// `process_file` calls so macro state accumulates naturally, then
// drop it when done.
class Preprocessor {
public:
    // The grammar provides the preprocessor's directive syntax
    // (e.g. loaded from `grammars/systemverilog.rawast`). The
    // options bundle controls runtime behavior; see PpOptions for
    // documentation of each field.
    Preprocessor(const Grammar& pp_grammar, PpOptions opts = {});

    // Non-copyable, non-movable. The Grammar reference makes move
    // assignment ill-defined and the state is heavy enough that
    // copying would be a footgun. Pass by reference to APIs that
    // need it; construct in place where you'll use it.
    Preprocessor(const Preprocessor&)            = delete;
    Preprocessor& operator=(const Preprocessor&) = delete;
    Preprocessor(Preprocessor&&)                 = delete;
    Preprocessor& operator=(Preprocessor&&)      = delete;

    // ─── Processing ─────────────────────────────────────────────────

    // Process inline text. State accumulates on the instance across
    // calls. Returns the preprocessed text.
    std::string process(const std::string& text);

    // Process a file from disk. Sets `state_.current_file` and
    // records the path in `included_files()` (first-seen order;
    // duplicates suppressed). Returns the preprocessed text.
    std::string process_file(const std::string& path);

    // ─── Inspection ─────────────────────────────────────────────────

    const std::unordered_map<std::string, MacroDef>&
    macros() const noexcept { return state_.macros; }

    const std::vector<std::string>&
    included_files() const noexcept { return state_.included_files; }

    const std::vector<PpWarning>&
    warnings() const noexcept { return state_.warnings; }

    const std::vector<Span>&
    spans() const noexcept { return state_.spans; }

    // Build a source-provenance frame stack for a byte offset in the
    // preprocessed output. The leaf frame describes the immediate
    // source; subsequent frames walk up the parent chain to the
    // ultimate root (typically the entry input file). Returns an
    // empty vector if the offset is past the end of recorded spans
    // (e.g. synthetic content with no provenance).
    std::vector<SourceFrame>
    stack_at(std::size_t out_offset) const;

    // ─── Query ──────────────────────────────────────────────────────

    bool is_defined(const std::string& name) const noexcept;
    const MacroDef* get_macro(const std::string& name) const noexcept;

    // ─── State management ───────────────────────────────────────────

    PreprocessorState snapshot() const { return state_; }
    void restore(PreprocessorState state) { state_ = std::move(state); }
    void reset() { state_ = {}; }

    // ─── Convenience wiring ────────────────────────────────────────

    // Install (or clear, with an empty function) the `\`if` condition
    // evaluator after construction. Mirrors PpOptions::expr_eval; lets a
    // host wire a callback that refers back to this Preprocessor without
    // the construct-time chicken-and-egg.
    void set_expr_eval(std::function<ValuePtr(const ValuePtr&)> fn) {
        opts_.expr_eval = std::move(fn);
    }

    // The undecidable-condition policy, settable after construction.
    void set_on_undecidable(PpOnUndecidable v) { opts_.on_undecidable = v; }

private:
    // The engine's BUILT-IN `\`if`/`\`elsif` evaluator: runs the structured
    // condition AST through default_pp_expr_eval with C `#if` semantics,
    // resolving macro refs against this Preprocessor's own table. Used at
    // the `\`if` site whenever no custom opts_.expr_eval is set — so the
    // preprocessor grammar (which parses the condition into an AST) gets
    // standard evaluation with no host wiring.
    ValuePtr eval_cond_default(const ValuePtr& cond);

    // The scan driver: the preprocessor's core mechanism. Walks `text`
    // once, and at each backtick matches ONE construct via the grammar's
    // PP_CONSTRUCT rule (parse_from). Strings, comments, and plain text
    // are passed through verbatim to `out`; recognised directives are
    // evaluated and macro uses expanded. A conditional emit/skip stack
    // (local to the call) gates output through `\`ifdef`/`\`else` chains.
    // `\`include` recurses into scan_stream on the included text.
    void scan_stream(const std::string& text, std::string& out);

    // Expand a single `\`NAME[(args)]` use at the top level: defined →
    // render its (recursively-expanded) body; undefined → apply the
    // undefined_handler then the on_undefined policy. Returns the text
    // to emit.
    std::string expand_macro_use(const class DictValue& d);

    // Per-role handlers. Defined here rather than as free functions
    // so they have direct access to state_ and opts_.
    void handle_define(const class DictValue& d);
    void handle_undef(const class DictValue& d);

    // Expand a `\`MACRO` use whose AST sits inside a substituted
    // macro body — no source-mapped cursor or span tracking, just
    // produce the text the use site would emit. Used by
    // `render_macro_body_segments` to walk macro_use AST nodes that
    // appeared inside the body of another macro after AST-level
    // parameter substitution. Mirrors the matching logic of
    // `handle_macro_use` (lookup, arity check, blue-paint cycle
    // guard, undefined_handler dispatch, on_undefined policy) but
    // emits to a string instead of `out`.
    std::string render_macro_use_inline(const class DictValue& d);

    // Render a (possibly post-substitution) macro body's segments
    // to text. Bare StringValue runs emit verbatim; macro_use dicts
    // recurse through `render_macro_use_inline`; other typed
    // segments fall back to `render_segment` for leaf text.
    std::string render_macro_body_segments(const class ArrayValue& segs);

    // `\`include "path"` — resolve the file (include_source callback,
    // then include_paths, then the including file's dir), and, in splice
    // mode, recursively scan_stream the included text into `out`. Macro
    // and included-files state mutate regardless of splice.
    void handle_include(const class DictValue& d, std::string& out);

    // Recursively expand inline `\`MACRO` / `\`MACRO(args)` references
    // within a body string, applying parameter substitution and
    // blue-paint cycle protection. Returns the fully expanded text.
    // Adds macro names to state_.active_expansions while their bodies
    // are being expanded; bumps state_.current_depth for the duration
    // and aborts (with a warning + verbatim passthrough) if the
    // configured max_expansion_depth is reached.
    std::string expand_recursive(const std::string& text);

    // Helpers for advancing src_cursor past consumed-but-not-emitted
    // source spans (directives like \`define / \`undef, dropped
    // ifdef bodies, the structural lines of conditional blocks).
    // Defined in the .cpp file.

    // Record an emitted run as a new Span entry. The new span is a
    // child of `parent_span_id` at `parent_offset`, sits in the
    // preprocessed output at `out_offset`, and is `length` bytes long.
    // Returns the new span's id.
    std::uint32_t record_span(std::uint32_t parent_id,
                              std::size_t parent_offset,
                              std::size_t length,
                              std::size_t out_offset,
                              std::string name);

    const Grammar&    pp_grammar_;
    PpOptions         opts_;
    PreprocessorState state_;
};

} // namespace rawast
