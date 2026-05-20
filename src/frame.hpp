#pragma once

#include <rawast/node.hpp>
#include <rawast/value.hpp>

#include <cstddef>
#include <vector>

namespace rawast {

class Grammar;
class ValuePool;

// Internal per-level state for the trampolined load driver. Snapshots the
// configuration of one Node and accumulates emitted values from descendant
// levels (the "catcher" pattern described in §3.4 of the proposal).
//
// Not part of the public API. Lives in src/.
class Frame {
public:
    struct EmittedValue {
        ValuePtr value;
        bool is_name = false;
    };

    Frame(const Grammar& g, NodeId node_id);

    NodeId node_id() const noexcept { return node_id_; }
    NodeKind kind() const noexcept { return kind_; }
    Container container() const noexcept { return container_; }
    bool is_optional() const noexcept { return is_optional_; }
    bool is_name() const noexcept { return is_name_; }
    bool has_separator() const noexcept { return has_separator_; }
    bool is_backtrack() const noexcept { return is_backtrack_; }

    // Backtracking-Choice bookkeeping: the driver records here whether
    // it has issued a StreamReader::mark() for the current alternative
    // attempt. The mark gets accepted on alternative success and
    // rejected on alternative failure.
    bool has_mark() const noexcept { return has_mark_; }
    void set_has_mark(bool v) noexcept { has_mark_ = v; }

    // Force the Frame's is_optional flag on. Used by push_node when a
    // Ref in the resolution chain carried is_optional=true that the
    // resolved Node itself doesn't have (so `?<RULE>` correctly makes
    // that one ref-site optional without mutating the rule).
    void force_optional() noexcept { is_optional_ = true; }

    void add_value(ValuePtr v, bool is_name);

    bool has_current() const noexcept;
    NodeId current_child() const;

    // Advance the child iterator. Returns true if there's another child
    // to process; for Repeat, wraps back to the first item (after the
    // separator) on overflow.
    bool step_next();

    // Materialise array/dict container from accumulated values; no-op for
    // Container::None. Registers container→child back-references on the
    // pool so post-parse value search can resolve "which containers hold
    // this value?" in O(1) lookup time.
    void finish(ValuePool& pool);

    // Move accumulated values into the parent frame's accumulator.
    void pass_values_to(Frame& parent);

    // For the top-level frame after finish(): return the single
    // accumulated value, or nullptr if none.
    ValuePtr result();

private:
    NodeId node_id_;
    NodeKind kind_;
    Container container_;
    bool is_optional_;
    bool is_name_;
    bool has_separator_;
    bool is_backtrack_;
    bool has_mark_ = false;
    std::vector<NodeId> children_;
    std::size_t child_idx_ = 0;
    std::vector<EmittedValue> emitted_;
};

} // namespace rawast
