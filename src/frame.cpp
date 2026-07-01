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

    // Value-kind frames pre-seed emitted_ with their own constant; the
    // driver pops them immediately after construction, so the constant
    // flows up to the parent at the correct positional moment in the
    // child-iteration order (not at frame-construction time, which is
    // what an absorber approach would do).
    if (kind_ == NodeKind::Value && n.value) {
        emitted_.push_back({n.value, is_name_});
    }

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

void Frame::add_value(ValuePtr v, bool is_name) {
    emitted_.push_back({std::move(v), is_name});
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

void Frame::finish(ValuePool&) {
    switch (container_) {
    case Container::None:
        return;

    case Container::Array: {
        auto arr = std::make_shared<ArrayValue>();
        auto& data = arr->data();
        data.reserve(emitted_.size());
        for (auto& ev : emitted_) data.push_back(ev.value);
        emitted_.clear();
        emitted_.push_back({arr, false});
        return;
    }

    case Container::Dict: {
        auto dict = std::make_shared<DictValue>();
        auto& map = dict->data();
        std::string current_name;
        bool have_name = false;
        for (auto& ev : emitted_) {
            if (ev.is_name) {
                auto sv = std::dynamic_pointer_cast<StringValue>(ev.value);
                if (sv) {
                    current_name = sv->data();
                    have_name = true;
                }
            } else if (have_name) {
                if (current_name.size() >= 2
                    && current_name.compare(current_name.size() - 2, 2, "[]") == 0) {
                    std::string base = current_name.substr(0, current_name.size() - 2);
                    auto& slot = map[base];
                    auto arr = std::dynamic_pointer_cast<ArrayValue>(slot);
                    if (!arr) {
                        arr = std::make_shared<ArrayValue>();
                        slot = arr;
                    }
                    arr->data().push_back(ev.value);
                } else {
                    map[current_name] = ev.value;
                }
                have_name = false;
            }
        }
        emitted_.clear();
        emitted_.push_back({dict, false});
        return;
    }
    }
}

void Frame::pass_values_to(Frame& parent) {
    for (auto& ev : emitted_) {
        parent.emitted_.push_back(std::move(ev));
    }
    emitted_.clear();
}

ValuePtr Frame::result() {
    if (emitted_.empty()) return nullptr;
    return emitted_.front().value;
}

} // namespace rawast
