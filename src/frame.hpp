#pragma once

#include <rawast/builder.hpp>
#include <rawast/node.hpp>
#include <rawast/value.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rawast {

class Grammar;
class ValuePool;

// Internal per-level state for the trampolined load driver. Snapshots the
// configuration of one Node: kind, container, child iteration, stream-mark
// bookkeeping. Values do NOT live here — the driver emits construction
// events to the Builder, which owns all value accumulation/materialisation.
//
// Not part of the public API. Lives in src/.
class Frame {
public:
    Frame(const Grammar& g, NodeId node_id);

    NodeId node_id() const noexcept { return node_id_; }
    NodeKind kind() const noexcept { return kind_; }
    Container container() const noexcept { return container_; }
    bool is_optional() const noexcept { return is_optional_; }
    bool is_negative() const noexcept { return is_negative_; }
    bool is_name() const noexcept { return is_name_; }
    bool has_separator() const noexcept { return has_separator_; }
    bool is_backtrack() const noexcept { return is_backtrack_; }

    // Repeat-only: how many full iterations have completed, and the
    // minimum required for the Repeat to succeed (default 0 = `*`; 1 for
    // the `repeat+` one-or-more form).
    std::uint32_t iter_count() const noexcept { return iter_count_; }
    std::uint32_t min() const noexcept { return min_; }

    // Backtracking-Choice bookkeeping: the driver records here whether
    // it has issued a StreamReader::mark() for the current alternative
    // attempt. The mark gets accepted on alternative success and
    // rejected on alternative failure.
    bool has_mark() const noexcept { return has_mark_; }
    void set_has_mark(bool v) noexcept { has_mark_ = v; }

    // Negative-lookahead bookkeeping. The neg-mark is the stream mark
    // taken at the moment a `!X` frame is pushed; on either the inner
    // matching or failing, the driver rejects this mark to restore the
    // cursor to that point (a successful `!X` consumes nothing; a
    // failing `!X` also consumes nothing, just for a different reason).
    // Kept separate from has_mark_ so a `!<CHOICE>` where the inner
    // Choice has its own backtrack mark doesn't collide — the two
    // marks serve different purposes and target different positions.
    bool has_neg_mark() const noexcept { return has_neg_mark_; }
    void set_has_neg_mark(bool v) noexcept { has_neg_mark_ = v; }

    // Shadow-builder checkpoint, taken just before this frame's begin() —
    // used to record the frame's contribution into the cache (record_from)
    // and to roll the builder back when the frame is unwound on failure.
    Builder::Checkpoint builder_cp() const noexcept { return builder_cp_; }
    void set_builder_cp(Builder::Checkpoint cp) noexcept { builder_cp_ = cp; }

    // Force the Frame's is_optional flag on. Used by push_node when a
    // Ref in the resolution chain carried is_optional=true that the
    // resolved Node itself doesn't have (so `?<RULE>` correctly makes
    // that one ref-site optional without mutating the rule).
    void force_optional() noexcept { is_optional_ = true; }

    // Force the Frame's is_negative flag on. Mirrors force_optional —
    // used by push_node when a `!<RULE>` ref-site needs to mark the
    // resolved body's frame negative without mutating the rule itself.
    void force_negative() noexcept { is_negative_ = true; }

    bool has_current() const noexcept;
    NodeId current_child() const;

    // Advance the child iterator. Returns true if there's another child
    // to process; for Repeat, wraps back to the first item (after the
    // separator) on overflow.
    bool step_next();

    // Sequence-success cache support. Set when the frame is pushed so
    // the driver can (a) retire interior cache entries appended while
    // this frame was active and (b) re-detect the entry point on cache
    // lookup for sibling-choice retries.
    std::size_t cache_size_at_push() const noexcept { return cache_size_at_push_; }
    void set_cache_size_at_push(std::size_t s) noexcept { cache_size_at_push_ = s; }
    std::size_t start_offset() const noexcept { return start_offset_; }
    void set_start_offset(std::size_t o) noexcept { start_offset_ = o; }

private:
    NodeId node_id_;
    NodeKind kind_;
    Container container_;
    bool is_optional_;
    bool is_negative_;
    bool is_name_;
    bool has_separator_;
    bool is_backtrack_;
    bool has_mark_ = false;
    bool has_neg_mark_ = false;
    std::vector<NodeId> children_;
    std::size_t child_idx_ = 0;
    std::uint32_t iter_count_ = 0;
    std::uint32_t min_ = 0;
    std::size_t cache_size_at_push_ = 0;
    std::size_t start_offset_ = 0;
    Builder::Checkpoint builder_cp_{};
};

} // namespace rawast
