#pragma once

#include <rawast/value.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace rawast {

// Eight kinds of grammar node. See §2.2 of the proposal for semantics.
enum class NodeKind {
    Ref,       // Named reference to another node.
    Value,     // Constant value emission.
    Key,       // Literal-string terminal.
    Parse,     // Invoke a named terminal parser.
    Choice,    // Ordered alternation; first matching alternative wins.
    Sequence,  // Concatenation of children.
    Repeat,    // Zero-or-more iteration of a child, optional separator.
    Raw,       // `*` in a sequence body: consume raw bytes until the next
               // sequence sibling (which must be a Key literal) matches at
               // the cursor. The literal itself is left unconsumed for the
               // sibling to match. The captured prefix is emitted as a
               // StringValue; ignore-set skipping is bypassed so embedded
               // whitespace and newlines round-trip verbatim. The stop
               // literal is stashed on `value` (StringValue) at load time.
    Scope,     // `scope { OPEN, INNER..., CLOSE }` — string-aware bracket
               // scanner. children[0] is OPEN (must be a Key), children.back()
               // is CLOSE (must be a Key); intermediate children are INNERs
               // (rule Refs or terminal-Parser invocations) that consume
               // atomic spans where embedded close bytes must not trigger
               // a false CLOSE match. The scan loop tries CLOSE first at
               // each step, then each INNER in order; on no match it
               // advances one raw byte. The captured body bytes (between
               // OPEN end and CLOSE start, exclusive) are emitted as a
               // StringValue. Ignore-set skipping is bypassed inside the
               // scope body so embedded whitespace round-trips verbatim.
};

// What kind of container the surrounding level materialises at end-of-frame.
enum class Container {
    None,
    Array,
    Dict,
};

// Preprocessor semantic role for a grammar rule, set via the `:#role="..."`
// engine-reserved binding. Annotates the rule with the conceptual operation
// the preprocessor walker should perform when it encounters a value
// produced by this rule. `None` (the default) means the rule has no
// preprocessor semantics and is treated as ordinary structural data.
//
// String-form names used in `:#role="..."` are the lowercased variant of
// each enumerator. See preprocessor.hpp for the string ↔ enum helpers.
enum class PpRole {
    None,
    Define,
    Undef,
    Ifdef,
    Ifndef,
    If,
    Elsif,
    Else,
    Endif,
    Include,
    MacroUse,
    Paste,
    Stringify,
    Text,
};

// Strong-typed handle into the Engine's node arena.
// Stable across arena growth (unlike list/vector iterators).
class NodeId {
    std::size_t idx_ = std::numeric_limits<std::size_t>::max();
public:
    constexpr NodeId() noexcept = default;
    constexpr explicit NodeId(std::size_t i) noexcept : idx_(i) {}

    constexpr std::size_t value() const noexcept { return idx_; }
    constexpr bool valid() const noexcept {
        return idx_ != std::numeric_limits<std::size_t>::max();
    }

    // Six relational operators expressed explicitly (C++17). Previously
    // a single `operator<=>(...) = default` (C++20) generated them. The
    // engine doesn't need C++20 for anything else, so this keeps the
    // build-toolchain bar low: C++17 = GCC 7+ / Clang 5+ / MSVC 2017+ /
    // manylinux2014. Easy backport if we ever pick C++20 for other
    // reasons; in the meantime, the explicit form is just as fast.
    constexpr bool operator==(const NodeId& o) const noexcept { return idx_ == o.idx_; }
    constexpr bool operator!=(const NodeId& o) const noexcept { return idx_ != o.idx_; }
    constexpr bool operator< (const NodeId& o) const noexcept { return idx_ <  o.idx_; }
    constexpr bool operator<=(const NodeId& o) const noexcept { return idx_ <= o.idx_; }
    constexpr bool operator> (const NodeId& o) const noexcept { return idx_ >  o.idx_; }
    constexpr bool operator>=(const NodeId& o) const noexcept { return idx_ >= o.idx_; }
};

// One node in the grammar tree. All fields default-construct to a safe
// inert sequence with no children — concrete kinds are set by the builder
// API (added in Phase 3).
class Node {
public:
    NodeKind  kind      = NodeKind::Sequence;
    Container container = Container::None;

    // The `var` flag from the prototype, moved here so that Value stays
    // immutable. Marks a Parse child as producing a dict-key name.
    bool is_name       = false;
    bool is_optional   = false;
    // Negative-lookahead marker: the node matches IFF its inner
    // production would FAIL at the current cursor, and consumes zero
    // input either way. Surface form `!X` in .rawast (where X is a
    // Ref, Key, or strict-Key item). Mutually meaningful with
    // is_optional only on Refs that resolve to the same body via the
    // chain — the parser distinguishes the two via separate frame
    // flags and separate stream marks, so they don't collide.
    bool is_negative   = false;
    bool has_separator = false;  // If true, children[0] is the separator.

    // Repeat-only: minimum number of successful iterations required. Default
    // 0 (the classical PEG `*` — zero-or-more). Set to 1 by the .rawast
    // `repeat+` form (one-or-more); the parse fails if fewer than `min`
    // iterations matched.
    std::uint32_t min = 0;

    // Opt-in structural backtracking. Currently only meaningful on Choice
    // nodes: each alternative attempt is wrapped in StreamReader::mark()
    // / reject() so that alternatives can share a leading-terminal prefix
    // (the EDA `+ FOO / + BAR / + BAZ` pattern). Off by default.
    bool backtrack     = false;

    // Save-direction: force fixed-schema scope push on a Sequence with
    // container=Dict, even when has_name_markers can't detect the named
    // fields (e.g. because they live inside a referenced sub-rule). Set
    // by the grammar via `"fixed_schema": true` in JSON form.
    bool fixed_schema  = false;

    // `#opchain` reserved flag — when set on a rule body's node, the
    // parse engine post-processes the produced AST: any dict in the
    // subtree matching always-wrap shape `{lhs, tail:[{op,rhs}, ...]}`
    // is compacted to `{op, args[]}` (same-op runs collapse flat,
    // mixed-op boundaries nest). Save-side reverses the transform
    // before normal dispatch. See `Grammar::set_opchain`.
    bool opchain       = false;

    // Key-only: word-boundary strict matching. When set on a Key node,
    // the KeyParser additionally checks that the byte immediately after
    // the matched literal is NOT a word character (alphanumeric or
    // underscore) — preventing the classic "not" matching the prefix of
    // "notch" bug. The boundary check fires only if the last character
    // of the literal is itself a word character; for punctuation keys
    // (`+`, `(`, `;`) the flag has no effect since no word continuation
    // is possible. Set by the grammar via `"strict": true` in JSON form
    // or `'token'` (single-quote literal) in `.rawast` DSL form.
    bool strict        = false;

    // Scope's stop literal is sibling-driven: `resolve_raw_stops`
    // (which also handles Scope nodes) copies the next sibling Key's
    // literal onto Node.value at load time — the same field Raw uses
    // for its sibling-stop. Scope has no opening delimiter of its
    // own; the surrounding sequence's siblings carry the start and
    // stop literals. Strict (word-bounded) matching is selected by
    // the sibling Key's `strict` flag, not by any attribute on the
    // scope itself. Net: scope is structurally "Raw with INNERs and
    // optional array container."

    // Carried for Key, Parse, Value kinds.
    //   Key   - StringValue holding the literal token to match.
    //   Parse - StringValue holding the name of the terminal parser to invoke.
    //   Value - any Value, emitted directly when the surrounding branch fires.
    ValuePtr value;

    // Raw/Scope stop literals — the set of Key-literal byte strings that
    // terminate the byte-scan when matched at the cursor. Populated by
    // the load-time stop resolver (see `resolve_raw_stops` in
    // src/loader.cpp).
    //
    // Single-stop case (`sequence { OPEN, scope { ... }, CLOSE }`) → one
    // entry: the next sibling Key's literal.
    //
    // Multi-stop case (`sequence { OPEN, repeat scope { ... } separator
    // SEP, CLOSE }`) → two entries: the repeat's separator literal and
    // the post-repeat Key literal. Either marks a legitimate boundary
    // — non-final iterations terminate at SEP, the final iteration at
    // CLOSE.
    //
    // For backward-compatible introspection / save round-trip, `value`
    // is also kept set to the FIRST stop literal in the single-stop
    // case (the dominant historical form). Multi-stop save round-trip
    // is a separate piece — preprocessor-side grammars own their files,
    // so it isn't on the critical path.
    std::vector<std::string> stops;

    // Save-direction pretty-print metadata. All ignored by parse.
    //
    // The save direction emits, for each Node entered:
    //   [ if indent_emit:  depth × Grammar::indent_step() ]
    //   [ recursive content ]
    //   [ if tail.size():   tail ]
    //   [ if space_after:   " " ]
    //   [ if newline_after: "\n" ]
    //
    // and bumps depth by +1 around the recursive content if `depth_in`
    // is set (popping on exit).
    //
    // Names map to the .rawast postfix keywords:
    //   depth_in     <- `indent`
    //   indent_emit  <- `tab`
    //   space_after  <- `space`
    //   newline_after<- `newline`
    //   tail         <- `tail="..."`
    bool        depth_in      = false;
    bool        indent_emit   = false;
    bool        space_after   = false;
    bool        newline_after = false;
    std::string tail;

    // Subparse re-entry. When set on a Parse-kind node, after the terminal
    // parser succeeds and returns a StringValue, the engine invokes the
    // parse loop again on that string starting from `subparse_start` —
    // same grammar, different entry rule. The resulting value replaces
    // the original string in the value stream. Invalid (default) means
    // no subparse. Set by the .rawast `:#subparse=<RULE>` binding.
    NodeId subparse_start;

    // Preprocessor semantic role for this rule, set by `:#role="..."`.
    // None (the default) means no preprocessor semantics. When the
    // Preprocessor walker visits a value produced by this node, it
    // dispatches on this role to apply the corresponding operation
    // (register a macro, expand a use, branch on an ifdef, etc.).
    PpRole pp_role = PpRole::None;

    std::vector<NodeId> children;
};

} // namespace rawast
