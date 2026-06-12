#pragma once

#include <rawast/node.hpp>

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
};

// Forward declaration so Preprocessor can hold a Grammar reference
// without dragging the full grammar.hpp into every translation unit
// that includes preprocessor.hpp.
class Grammar;

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

    // Macro expansion recursion limit. Protects against cycles
    // that blue-painting alone doesn't catch (e.g. mutual
    // expansion via include + redefine). Hitting the limit
    // aborts processing with a PpError.
    int max_expansion_depth = 200;

    // Capture per-directive events into trace storage. Useful for
    // tooling and debugging; default off keeps the hot path cheap.
    bool trace = false;
};

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

    // ─── Inspection ─────────────────────────────────────────────────

    const std::unordered_map<std::string, MacroDef>&
    macros() const noexcept { return state_.macros; }

    const std::vector<std::string>&
    included_files() const noexcept { return state_.included_files; }

    const std::vector<PpWarning>&
    warnings() const noexcept { return state_.warnings; }

    // ─── Query ──────────────────────────────────────────────────────

    bool is_defined(const std::string& name) const noexcept;
    const MacroDef* get_macro(const std::string& name) const noexcept;

    // ─── State management ───────────────────────────────────────────

    PreprocessorState snapshot() const { return state_; }
    void restore(PreprocessorState state) { state_ = std::move(state); }
    void reset() { state_ = {}; }

private:
    const Grammar&    pp_grammar_;
    PpOptions         opts_;
    PreprocessorState state_;
};

} // namespace rawast
