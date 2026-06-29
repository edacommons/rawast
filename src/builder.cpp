#include "builder.hpp"

#include <rawast/pool.hpp>

#include <memory>
#include <utility>

namespace rawast {

SharedPtrBuilder::SharedPtrBuilder(ValuePool& pool) : pool_(pool) {
    levels_.push_back({Container::None, {}});   // root
}

void SharedPtrBuilder::value(ValuePtr v, bool is_name) {
    levels_.back().emitted.push_back({std::move(v), is_name});
}

void SharedPtrBuilder::begin(Container kind) {
    levels_.push_back({kind, {}});
}

void SharedPtrBuilder::end() {
    Level lvl = std::move(levels_.back());
    levels_.pop_back();
    auto& parent = levels_.back().emitted;

    // Materialisation lifted from Frame::finish.
    switch (lvl.kind) {
    case Container::None:
        for (auto& ev : lvl.emitted) parent.push_back(std::move(ev));
        return;

    case Container::Array: {
        auto arr = std::make_shared<ArrayValue>();
        auto& data = arr->data();
        data.reserve(lvl.emitted.size());
        ValuePtr container_ptr = arr;
        for (auto& ev : lvl.emitted) {
            data.push_back(ev.value);
            pool_.register_usage(ev.value, container_ptr);
        }
        parent.push_back({arr, false});
        return;
    }

    case Container::Dict: {
        auto dict = std::make_shared<DictValue>();
        auto& map = dict->data();
        ValuePtr container_ptr = dict;
        std::string current_name;
        bool have_name = false;
        for (auto& ev : lvl.emitted) {
            if (ev.is_name) {
                if (auto sv = std::dynamic_pointer_cast<StringValue>(ev.value)) {
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
                        pool_.register_usage(arr, container_ptr);
                    }
                    arr->data().push_back(ev.value);
                    pool_.register_usage(ev.value, arr);
                } else {
                    map[current_name] = ev.value;
                    pool_.register_usage(ev.value, container_ptr);
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
    while (levels_.size() > cp.depth) levels_.pop_back();
    levels_.back().emitted.resize(cp.size);
}

ValuePtr SharedPtrBuilder::result() const {
    if (levels_.front().emitted.empty()) return nullptr;
    return levels_.front().emitted.front().value;
}

} // namespace rawast
