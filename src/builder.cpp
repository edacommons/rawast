#include "builder.hpp"

#include <rawast/pool.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

namespace rawast {

static const bool g_strace = std::getenv("RAWAST_SHADOW_TRACE") != nullptr;

SharedPtrBuilder::SharedPtrBuilder(ValuePool& pool) : pool_(pool) {
    levels_.push_back({Container::None, {}});   // root
}

void SharedPtrBuilder::value(ValuePtr v, bool is_name) {
    if (g_strace) std::fprintf(stderr, "  shadow VALUE depth=%zu name=%d\n",
                               levels_.size(), is_name);
    levels_.back().emitted.push_back({std::move(v), is_name});
}

void SharedPtrBuilder::begin(Container kind) {
    if (g_strace) std::fprintf(stderr, "  shadow BEGIN(%d) -> depth=%zu\n",
                               static_cast<int>(kind), levels_.size() + 1);
    levels_.push_back({kind, {}});
}

void SharedPtrBuilder::end() {
    if (g_strace) std::fprintf(stderr, "  shadow END depth=%zu size=%zu\n",
                               levels_.size(),
                               levels_.empty() ? 0 : levels_.back().emitted.size());
    if (levels_.size() < 2) return;   // desync guard (no open container)
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
    if (g_strace) std::fprintf(stderr, "  shadow ROLLBACK to depth=%zu size=%zu (from depth=%zu)\n",
                               cp.depth, cp.size, levels_.size());
    std::size_t depth = cp.depth < 1 ? 1 : cp.depth;   // never drop the root
    while (levels_.size() > depth) levels_.pop_back();
    auto& emitted = levels_.back().emitted;
    if (cp.size <= emitted.size()) emitted.resize(cp.size);
}

SharedPtrBuilder::Recording
SharedPtrBuilder::record_from(Checkpoint cp) const {
    if (cp.depth < 1 || cp.depth > levels_.size()) return {};   // desync guard
    const auto& lvl = levels_[cp.depth - 1].emitted;
    if (cp.size > lvl.size()) return {};
    return Recording(lvl.begin() + cp.size, lvl.end());
}

void SharedPtrBuilder::replay(const Recording& rec) {
    for (const auto& ev : rec) levels_.back().emitted.push_back(ev);
}

ValuePtr SharedPtrBuilder::result() const {
    if (levels_.front().emitted.empty()) return nullptr;
    return levels_.front().emitted.front().value;
}

} // namespace rawast
