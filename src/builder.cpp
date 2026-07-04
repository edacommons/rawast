#include "builder.hpp"

#include <rawast/pool.hpp>

#include <cstdio>
#include <memory>
#include <utility>

namespace rawast {

// ---------------------------------------------------------------------------
// Builder — default adopt(): translate a reference-model subtree into typed
// events, so a plug-in builder only ever implements the typed surface.
// ---------------------------------------------------------------------------

void Builder::adopt(const ValuePtr& v, bool is_name) {
    if (!v) { null_(is_name); return; }
    switch (v->type()) {
    case ValueType::Null:      null_(is_name); return;
    case ValueType::Undefined: null_(is_name); return;
    case ValueType::Bool:
        bool_(static_cast<const BoolValue&>(*v).data(), is_name); return;
    case ValueType::Int:
        int_(static_cast<const IntValue&>(*v).data(), is_name); return;
    case ValueType::UInt:
        uint_(static_cast<const UIntValue&>(*v).data(), is_name); return;
    case ValueType::Real:
        real_(static_cast<const RealValue&>(*v).data(), is_name); return;
    case ValueType::String:
        string_(static_cast<const StringValue&>(*v).data(), is_name); return;
    case ValueType::Array: {
        begin(Container::Array);
        for (const auto& e : static_cast<const ArrayValue&>(*v).data())
            adopt(e, false);
        end();
        return;
    }
    case ValueType::Dict: {
        begin(Container::Dict);
        for (const auto& [k, val] : static_cast<const DictValue&>(*v).data()) {
            string_(k, true);
            adopt(val, false);
        }
        end();
        return;
    }
    }
}

// ---------------------------------------------------------------------------
// SharedPtrBuilder
// ---------------------------------------------------------------------------

SharedPtrBuilder::SharedPtrBuilder(ValuePool& pool) : pool_(pool) {
    levels_.push_back({Container::None, {}});   // root
}

void SharedPtrBuilder::push(ValuePtr v, bool is_name) {
    levels_.back().emitted.push_back({std::move(v), is_name});
}

// Typed leaves intern through the pool: identical primitives share one
// canonical ValuePtr across the whole parse (the sharing strategy that
// used to live in the driver).
void SharedPtrBuilder::null_(bool is_name)   { push(null_value(), is_name); }
void SharedPtrBuilder::bool_(bool v, bool is_name) {
    push(v ? true_value() : false_value(), is_name);
}
void SharedPtrBuilder::int_(std::int64_t v, bool is_name) {
    push(pool_.intern_int(v), is_name);
}
void SharedPtrBuilder::uint_(std::uint64_t v, bool is_name) {
    push(pool_.intern_uint(v), is_name);
}
void SharedPtrBuilder::real_(double v, bool is_name) {
    push(pool_.intern_real(v), is_name);
}
void SharedPtrBuilder::string_(std::string_view v, bool is_name) {
    push(pool_.intern_string(std::string(v)), is_name);
}

// Zero-copy adoption: primitives re-intern (dedup, reusing the incoming
// allocation on first sight); composites are shared as-is.
void SharedPtrBuilder::adopt(const ValuePtr& v, bool is_name) {
    if (!v) { push(nullptr, is_name); return; }
    switch (v->type()) {
    case ValueType::Array:
    case ValueType::Dict:
        push(v, is_name);
        return;
    default:
        push(pool_.intern(v), is_name);
        return;
    }
}

void SharedPtrBuilder::begin(Container kind) {
    levels_.push_back({kind, {}});
}

void SharedPtrBuilder::end() {
    if (levels_.size() < 2) return;   // desync guard (no open container)
    Level lvl = std::move(levels_.back());
    levels_.pop_back();
    auto& parent = levels_.back().emitted;

    switch (lvl.kind) {
    case Container::None:
        for (auto& ev : lvl.emitted) parent.push_back(std::move(ev));
        return;

    case Container::Array: {
        auto arr = std::make_shared<ArrayValue>();
        auto& data = arr->data();
        data.reserve(lvl.emitted.size());
        for (auto& ev : lvl.emitted) data.push_back(ev.value);
        parent.push_back({arr, false});
        return;
    }

    case Container::Dict: {
        auto dict = std::make_shared<DictValue>();
        auto& map = dict->data();
        std::string current_name;
        bool have_name = false;
        for (auto& ev : lvl.emitted) {
            if (ev.is_name) {
                if (auto sv = as_string(ev.value)) {
                    current_name = sv->data();
                    have_name = true;
                }
            } else if (have_name) {
                if (current_name.size() >= 2
                    && current_name.compare(current_name.size() - 2, 2, "[]") == 0) {
                    std::string base = current_name.substr(0, current_name.size() - 2);
                    auto& slot = map[base];
                    auto arr = as_array(slot);
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
        parent.push_back({dict, false});
        return;
    }
    }
}

Builder::Checkpoint SharedPtrBuilder::checkpoint() const {
    return {levels_.size(), levels_.back().emitted.size()};
}

void SharedPtrBuilder::rollback(Checkpoint cp) {
    std::size_t depth = cp.depth < 1 ? 1 : cp.depth;   // never drop the root
    while (levels_.size() > depth) levels_.pop_back();
    auto& emitted = levels_.back().emitted;
    if (cp.size <= emitted.size()) emitted.resize(cp.size);
}

Builder::Recording
SharedPtrBuilder::record_from(Checkpoint cp) const {
    if (cp.depth < 1 || cp.depth > levels_.size()) return {};   // desync guard
    const auto& lvl = levels_[cp.depth - 1].emitted;
    if (cp.size > lvl.size()) return {};
    return std::make_shared<const std::vector<EmittedValue>>(
        lvl.begin() + cp.size, lvl.end());
}

void SharedPtrBuilder::replay(const Recording& rec) {
    if (!rec) return;
    const auto& evs =
        *static_cast<const std::vector<EmittedValue>*>(rec.get());
    for (const auto& ev : evs) levels_.back().emitted.push_back(ev);
}

ValuePtr SharedPtrBuilder::result() const {
    if (levels_.front().emitted.empty()) return nullptr;
    return levels_.front().emitted.front().value;
}

} // namespace rawast
