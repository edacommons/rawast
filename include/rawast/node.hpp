#pragma once

#include <rawast/value.hpp>

#include <compare>
#include <cstddef>
#include <limits>
#include <vector>

namespace rawast {

// Seven kinds of grammar node. See §2.2 of the proposal for semantics.
enum class NodeKind {
    Ref,       // Named reference to another node.
    Value,     // Constant value emission.
    Key,       // Literal-string terminal.
    Parse,     // Invoke a named terminal parser.
    Choice,    // Ordered alternation; first matching alternative wins.
    Sequence,  // Concatenation of children.
    Repeat,    // Zero-or-more iteration of a child, optional separator.
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

    constexpr auto operator<=>(const NodeId&) const noexcept = default;
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

    // Carried for Key, Parse, Value kinds.
    //   Key   - StringValue holding the literal token to match.
    //   Parse - StringValue holding the name of the terminal parser to invoke.
    //   Value - any Value, emitted directly when the surrounding branch fires.
    ValuePtr value;

    std::vector<NodeId> children;
};

} // namespace rawast
