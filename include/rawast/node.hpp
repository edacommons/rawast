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
};

// What kind of container the surrounding level materialises at end-of-frame.
enum class Container {
    None,
    Array,
    Dict,
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

    // Carried for Key, Parse, Value kinds.
    //   Key   - StringValue holding the literal token to match.
    //   Parse - StringValue holding the name of the terminal parser to invoke.
    //   Value - any Value, emitted directly when the surrounding branch fires.
    ValuePtr value;

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
    // no subparse. Set by the .rawast `subparse=<RULE>` postfix attr.
    NodeId subparse_start;

    std::vector<NodeId> children;
};

} // namespace rawast
