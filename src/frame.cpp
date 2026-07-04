#include "frame.hpp"
#include "builder.hpp"
#include <rawast/grammar.hpp>
#include <rawast/pool.hpp>

#include <cassert>
#include <utility>

namespace rawast {

Frame::Frame(const Grammar& g, NodeId node_id) : node_id_(node_id) {
    const Node& n   = g.node(node_id);
    kind_           = n.kind;
    // Scope is a terminal in the Frame model — walk_scope_helper builds
    // the entire produced value (StringValue for default mode, or
    // ArrayValue of segments for `scope array`) and the Frame just
    // passes it through. Reusing n.container=Array to *also* trigger
    // the Frame-level array-of-children wrap would double-wrap the
    // segment array. Force None for Scope frames.
    container_      = (n.kind == NodeKind::Scope)
                      ? Container::None
                      : n.container;
    is_optional_    = n.is_optional;
    is_negative_    = n.is_negative;
    is_name_        = n.is_name;
    has_separator_  = n.has_separator;
    // Backtrack is default-on for Choice frames (standard PEG semantics:
    // each alternative attempt is wrapped in mark/reject so a partial
    // match can rewind cleanly). The Node::backtrack flag is retained
    // for grammar/lint introspection but no longer gates runtime
    // behavior on Choice.
    is_backtrack_   = (kind_ == NodeKind::Choice) || n.backtrack;
    min_            = n.min;

    // All children — including Value-kind — go into children_ for the
    // driver to iterate. This preserves positional interleaving between
    // Value-kind name markers and value-producing siblings within a
    // dict-container Sequence (the .rawast `name=@` binding pattern).
    for (NodeId child_id : n.children) {
        children_.push_back(child_id);
    }

    // Repeat-with-separator: children_[0] is the separator. Start past it
    // so the first iteration matches an item, not a separator.
    if (kind_ == NodeKind::Repeat && has_separator_ && !children_.empty()) {
        child_idx_ = 1;
    }
}

bool Frame::has_current() const noexcept {
    return child_idx_ < children_.size();
}

NodeId Frame::current_child() const {
    assert(has_current());
    return children_[child_idx_];
}

bool Frame::step_next() {
    if (child_idx_ < children_.size()) {
        ++child_idx_;
    }
    if (child_idx_ >= children_.size()) {
        if (kind_ == NodeKind::Repeat) {
            ++iter_count_;
            // Restart after a full pass. With a separator at children_[0]
            // it must be matched between iterations, so don't skip it.
            child_idx_ = 0;
            return !children_.empty();
        }
        return false;
    }
    return true;
}

} // namespace rawast
