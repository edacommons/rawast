#pragma once

#include <rawast/node.hpp>

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

// One macro definition. Function-like macros carry a non-empty
// `params` list AND `is_function_like = true`; an empty `params`
// list with `is_function_like = false` is the object-like form.
// `body` is the raw body text captured by the grammar; substitution
// happens at expansion time (see preprocessor_macro_lex.cpp once it
// lands). For Phase 1.2 only object-like macros are supported; the
// function-like fields are present in the struct so we don't churn
// the layout when function-like support arrives in Phase 2.
struct MacroDef {
    std::string name;
    std::vector<std::string> params;
    std::string body;
    bool is_function_like = false;
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
    std::function<std::optional<bool>(const ValuePtr& cond)> expr_eval;

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
// the documented expression-AST shape (see grammars/sv_pp_expr.rawast)
// and returns a tri-state result:
//
//   true   — condition holds
//   false  — condition does not hold
//   nullopt — undecidable (unknown call name, non-int operand of an
//            arithmetic op, ref without a resolver) — the walker
//            records a warning and treats it as false; hosts can
//            elevate via their own policy.
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
//                               // in grammars/sv_pp_expr.rawast
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
std::optional<bool> default_pp_expr_eval(
    const ValuePtr& cond,
    const std::function<std::optional<std::string>(
        const std::string& name)>& ref_resolver);

// The user-facing entry point for preprocessing. Owns the active
// PreprocessorState; orchestrates parse + walk for the configured
// preprocessor grammar (e.g. `sv_preprocessor`).
//
// Lifetimes: the Grammar reference must outlive the Preprocessor.
// Typical pattern is to construct the Preprocessor with a Grammar
// loaded by the caller, reuse the Preprocessor across many
// `process_file` calls so macro state accumulates naturally, then
// drop it when done.
class Preprocessor {
public:
    // The grammar provides the preprocessor's directive syntax
    // (e.g. loaded from `grammars/sv_preprocessor.rawast`). The
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

    // Walk a pre-built AST. Bypasses the grammar layer entirely —
    // useful for unit-testing the walker (and the dynamic_macros /
    // expr_eval / undefined_handler callbacks) against synthesized
    // ASTs, and for tooling that builds the AST through some other
    // mechanism (programmatic, deserialized from JSON, etc.).
    //
    // `source` is the byte string the walker should treat as the
    // original input — used by `locate_item` to position spans and
    // emit text. For synthesized ASTs that don't correspond to a
    // real source file, pass a synthetic string containing each
    // referenced token (the helpers `synth_text` etc. in the test
    // file build conforming sources). For ASTs deserialized from a
    // real file, pass that file's contents.
    //
    // Returns the preprocessed text. State accumulates on the
    // instance across calls, same as process().
    std::string process_ast(const ValuePtr& ast, const std::string& source);

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

    // Bind opts_.expr_eval to the generic AST evaluator above,
    // resolving refs through this Preprocessor's macro table. Use
    // when the preprocessor grammar produces the documented
    // expression-AST shape and you don't need custom evaluation
    // semantics on top of it.
    //
    //   Preprocessor pp(grammar);
    //   pp.use_default_expr_eval();
    //   auto out = pp.process(text);
    //
    // Overwrites any previously-set expr_eval callback. To extend
    // the default (e.g. add custom `call` handlers), wire your own
    // expr_eval that delegates to default_pp_expr_eval for the
    // shapes it doesn't handle.
    void use_default_expr_eval();

    // Same as use_default_expr_eval() but for preprocessor grammars
    // that capture `\`if` conditions as raw text (the
    // sv_preprocessor.rawast shape). When the walker hands cond in as
    // a StringValue, the callback parses it with `expr_grammar` first
    // (typically loaded from grammars/sv_pp_expr.rawast), then feeds
    // the resulting AST through default_pp_expr_eval.
    //
    //   Grammar pp = load("grammars/sv_preprocessor.rawast");
    //   Grammar ex = load("grammars/sv_pp_expr.rawast");
    //   Preprocessor p(pp);
    //   p.use_default_expr_eval(ex);
    //
    // Cond values that are already structured (DictValue/ArrayValue)
    // bypass the parse step and go straight to default_pp_expr_eval —
    // so the same Preprocessor handles both grammar-captured text and
    // process_ast-supplied synthesized ASTs.
    //
    // `expr_grammar` is captured by reference and must outlive the
    // Preprocessor. A parse failure is recorded as a warning and the
    // branch is treated as false (std::nullopt return).
    void use_default_expr_eval(const Grammar& expr_grammar);

private:
    // Walk the value tree produced by parsing through pp_grammar_,
    // dispatching on the `type` field that role-bearing rules emit.
    // Appends emitted text to `out`. Tracks the cursor into the
    // ORIGINAL source so each emitted run can record its provenance
    // span. `parent_span_id` is the parent Span every emitted span
    // should reference (the root input span for top-level walks; a
    // macro-body span when expansion lands in Phase 2.1).
    void walk(const ValuePtr& v, std::string& out,
              std::size_t& src_cursor,
              const std::string& source,
              std::uint32_t parent_span_id);

    // Per-role handlers. Defined here rather than as free functions
    // so they have direct access to state_ and opts_.
    void handle_define(const class DictValue& d);
    void handle_undef(const class DictValue& d);
    void handle_macro_use(const class DictValue& d, std::string& out,
                          std::size_t& src_cursor,
                          const std::string& source,
                          std::uint32_t parent_span_id);
    void handle_ifdef(const class DictValue& d, std::string& out,
                      std::size_t& src_cursor,
                      const std::string& source,
                      std::uint32_t parent_span_id,
                      bool invert);
    void handle_if(const class DictValue& d, std::string& out,
                   std::size_t& src_cursor,
                   const std::string& source,
                   std::uint32_t parent_span_id);
    void handle_include(const class DictValue& d, std::string& out,
                        std::size_t& src_cursor,
                        const std::string& source,
                        std::uint32_t parent_span_id);

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

    // Scoped guard set by `\`ifdef`/`\`ifndef`/`\`if` handlers when
    // walking a NOT-TAKEN branch. The walker still traverses the
    // branch (so src_cursor advances over the source bytes and
    // nested directives' shapes are visited), but per-directive
    // handlers check this flag and skip state mutations: handle_define
    // doesn't register the macro, handle_undef doesn't erase, etc.
    // Source-map spans and output bytes are also discarded by
    // walk_or_discard, but those are already routed through a
    // disposable buffer; this flag closes the macro-table gap.
    bool suppress_side_effects_ = false;
};

} // namespace rawast
