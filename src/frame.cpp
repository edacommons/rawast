#include "frame.hpp"
#include <rawast/grammar.hpp>

#include <cassert>
#include <utility>

namespace rawast {

Frame::Frame(const Grammar& g, NodeId node_id) : node_id_(node_id) {
    const Node& n   = g.node(node_id);
    kind_           = n.kind;
    container_      = n.container;
    is_optional_    = n.is_optional;
    is_name_        = n.is_name;
    has_separator_  = n.has_separator;

    // Absorb Value-kind children as constants directly into the catcher.
    // Other children get iterated by the driver.
    for (NodeId child_id : n.children) {
        NodeId resolved = g.resolve_ref(child_id);
        const Node& child = g.node(resolved);
        if (child.kind == NodeKind::Value && child.value) {
            emitted_.push_back({child.value, false});
        } else {
            children_.push_back(child_id);
        }
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
            child_idx_ = 0;
            return !children_.empty();
        }
        return false;
    }
    return true;
}

void Frame::finish() {
    switch (container_) {
    case Container::None:
        return;

    case Container::Array: {
        auto arr = std::make_shared<ArrayValue>();
        auto& data = arr->data();
        data.reserve(emitted_.size());
        for (auto& ev : emitted_) {
            data.push_back(std::move(ev.value));
        }
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
                map[current_name] = std::move(ev.value);
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
