#include <rawast/pool.hpp>

#include <utility>

namespace rawast {

ValuePtr ValuePool::intern(ValuePtr v) {
    if (!v) return v;

    switch (v->type()) {
    case ValueType::Null:
    case ValueType::Bool:
        // Already singletons (null_value / true_value / false_value).
        return v;

    case ValueType::Int: {
        auto iv = std::dynamic_pointer_cast<IntValue>(v);
        if (!iv) return v;
        auto [it, inserted] = ints_.try_emplace(iv->data(), v);
        return it->second;
    }

    case ValueType::UInt: {
        auto uv = std::dynamic_pointer_cast<UIntValue>(v);
        if (!uv) return v;
        auto [it, inserted] = uints_.try_emplace(uv->data(), v);
        return it->second;
    }

    case ValueType::Real: {
        auto rv = std::dynamic_pointer_cast<RealValue>(v);
        if (!rv) return v;
        auto [it, inserted] = reals_.try_emplace(rv->data(), v);
        return it->second;
    }

    case ValueType::String: {
        auto sv = std::dynamic_pointer_cast<StringValue>(v);
        if (!sv) return v;
        auto [it, inserted] = strings_.try_emplace(sv->data(), v);
        return it->second;
    }

    case ValueType::Array:
    case ValueType::Dict:
        // Composite hash-consing is opt-in and deferred. Pass through.
        return v;
    }
    return v;
}

ValuePtr ValuePool::intern_int(std::int64_t v) {
    return intern(make_int(v));
}

ValuePtr ValuePool::intern_uint(std::uint64_t v) {
    return intern(make_uint(v));
}

ValuePtr ValuePool::intern_real(double v) {
    return intern(make_real(v));
}

ValuePtr ValuePool::intern_string(std::string v) {
    return intern(make_string(std::move(v)));
}

void ValuePool::register_usage(ValuePtr value, ValuePtr container) {
    if (!value || !container) return;
    back_refs_[value.get()].push_back(std::move(container));
}

std::vector<ValuePtr> ValuePool::find_containers_of(const ValuePtr& value) const {
    if (!value) return {};
    auto it = back_refs_.find(value.get());
    if (it == back_refs_.end()) return {};
    return it->second;
}

std::size_t ValuePool::size() const noexcept {
    return ints_.size() + uints_.size() + reals_.size() + strings_.size();
}

} // namespace rawast
