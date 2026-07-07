#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rawast {

class Value;
using ValuePtr = std::shared_ptr<Value>;

// Dict backing store: a flat vector of (key, value) kept SORTED by key.
// Replaces std::map<std::string, ValuePtr> for DictValue. Rationale
// (measured): SV ASTs are ~2 entries/dict with only a handful of distinct
// keys; std::map spends ~half the tree's footprint on red-black nodes (3
// pointers + colour, separately malloc'd) each holding a 32-byte inline
// std::string key. A sorted vector stores the same pairs contiguously — no
// per-entry allocation, better locality — while preserving the KEY-SORTED
// iteration the Accessor/save contract relies on. Exposes the std::map
// subset call sites actually use, so it is a drop-in for DictValue::data().
class FlatDict {
public:
    using value_type     = std::pair<std::string, ValuePtr>;
    using storage        = std::vector<value_type>;
    using iterator       = storage::iterator;
    using const_iterator = storage::const_iterator;

    iterator       begin() noexcept       { return v_.begin(); }
    iterator       end() noexcept         { return v_.end(); }
    const_iterator begin() const noexcept { return v_.begin(); }
    const_iterator end() const noexcept   { return v_.end(); }
    std::size_t    size() const noexcept  { return v_.size(); }
    bool           empty() const noexcept { return v_.empty(); }
    void           clear() noexcept       { v_.clear(); }

    iterator find(std::string_view k) {
        auto it = lower(k);
        return (it != v_.end() && key_eq(it, k)) ? it : v_.end();
    }
    const_iterator find(std::string_view k) const {
        auto it = lower(k);
        return (it != v_.end() && key_eq(it, k)) ? it : v_.end();
    }
    std::size_t count(std::string_view k) const { return find(k) != end() ? 1 : 0; }

    ValuePtr& operator[](std::string_view k) {
        auto it = lower(k);
        if (it != v_.end() && key_eq(it, k)) return it->second;
        it = v_.insert(it, value_type{std::string(k), ValuePtr{}});
        return it->second;
    }
    ValuePtr& at(std::string_view k) {
        auto it = find(k);
        if (it == v_.end()) throw std::out_of_range("FlatDict::at");
        return it->second;
    }
    const ValuePtr& at(std::string_view k) const {
        auto it = find(k);
        if (it == v_.end()) throw std::out_of_range("FlatDict::at");
        return it->second;
    }

    std::pair<iterator, bool> emplace(std::string k, ValuePtr val) {
        auto it = lower(k);
        if (it != v_.end() && key_eq(it, k)) return {it, false};
        it = v_.insert(it, value_type{std::move(k), std::move(val)});
        return {it, true};
    }
    iterator    erase(const_iterator pos) { return v_.erase(pos); }
    std::size_t erase(std::string_view k) {
        auto it = find(k);
        if (it == v_.end()) return 0;
        v_.erase(it);
        return 1;
    }

private:
    static bool key_eq(const_iterator it, std::string_view k) {
        return std::string_view{it->first} == k;
    }
    iterator lower(std::string_view k) {
        return std::lower_bound(v_.begin(), v_.end(), k,
            [](const value_type& e, std::string_view kk) {
                return std::string_view{e.first} < kk;
            });
    }
    const_iterator lower(std::string_view k) const {
        return std::lower_bound(v_.begin(), v_.end(), k,
            [](const value_type& e, std::string_view kk) {
                return std::string_view{e.first} < kk;
            });
    }
    storage v_;
};

// Type tag carried by every Value.
enum class ValueType {
    Null,
    Undefined,
    Bool,
    Int,
    UInt,
    Real,
    String,
    Array,
    Dict,
};

// Abstract base for all values in the structural tree. Immutable post-
// construction; identity comparison via std::shared_ptr::get() is the
// substrate for the value pool described in §3.5 of the proposal.
class Value {
public:
    Value() = default;
    virtual ~Value() = default;

    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;

    virtual ValueType type() const noexcept = 0;
};

class NullValue final : public Value {
public:
    ValueType type() const noexcept override { return ValueType::Null; }
};

// Distinct from Null: a sentinel for "undefined" (vs an explicit null).
// Like Null it carries no payload and is a shared singleton; in Python it
// binds to the `Undefined` singleton so `val is Undefined` works.
class UndefinedValue final : public Value {
public:
    ValueType type() const noexcept override { return ValueType::Undefined; }
};

class BoolValue final : public Value {
    bool data_;
public:
    explicit BoolValue(bool v) noexcept : data_(v) {}
    ValueType type() const noexcept override { return ValueType::Bool; }
    bool data() const noexcept { return data_; }
};

class IntValue final : public Value {
    std::int64_t data_;
public:
    explicit IntValue(std::int64_t v) noexcept : data_(v) {}
    ValueType type() const noexcept override { return ValueType::Int; }
    std::int64_t data() const noexcept { return data_; }
};

class UIntValue final : public Value {
    std::uint64_t data_;
public:
    explicit UIntValue(std::uint64_t v) noexcept : data_(v) {}
    ValueType type() const noexcept override { return ValueType::UInt; }
    std::uint64_t data() const noexcept { return data_; }
};

class RealValue final : public Value {
    double data_;
public:
    explicit RealValue(double v) noexcept : data_(v) {}
    ValueType type() const noexcept override { return ValueType::Real; }
    double data() const noexcept { return data_; }
};

class StringValue final : public Value {
    std::string data_;
public:
    explicit StringValue(std::string v) noexcept : data_(std::move(v)) {}
    explicit StringValue(std::string_view v) : data_(v) {}
    ValueType type() const noexcept override { return ValueType::String; }
    const std::string& data() const noexcept { return data_; }
};

class ArrayValue final : public Value {
    std::vector<ValuePtr> data_;
public:
    ArrayValue() = default;
    explicit ArrayValue(std::vector<ValuePtr> v) noexcept : data_(std::move(v)) {}
    ValueType type() const noexcept override { return ValueType::Array; }
    const std::vector<ValuePtr>& data() const noexcept { return data_; }
    std::vector<ValuePtr>& data() noexcept { return data_; }
};

class DictValue final : public Value {
    FlatDict data_;
public:
    DictValue() = default;
    ValueType type() const noexcept override { return ValueType::Dict; }
    const FlatDict& data() const noexcept { return data_; }
    FlatDict& data() noexcept { return data_; }
};

// Shared singletons. Identity comparison is meaningful: null_value().get()
// is the same pointer everywhere null appears in any tree.
ValuePtr null_value();
ValuePtr undefined_value();
ValuePtr true_value();
ValuePtr false_value();

// Factory helpers. Primitive interning lives in the value pool (Phase 4);
// for now these always allocate a fresh value.
inline ValuePtr make_int(std::int64_t v) { return std::make_shared<IntValue>(v); }
inline ValuePtr make_uint(std::uint64_t v) { return std::make_shared<UIntValue>(v); }
inline ValuePtr make_real(double v) { return std::make_shared<RealValue>(v); }
// Single make_string overload — std::string is constructible from char* and
// from string_view, so callers don't have to pick. Avoids the literal-vs-view
// overload ambiguity entirely.
inline ValuePtr make_string(std::string v) { return std::make_shared<StringValue>(std::move(v)); }
inline ValuePtr make_array() { return std::make_shared<ArrayValue>(); }
inline ValuePtr make_dict() { return std::make_shared<DictValue>(); }

// Type-tagged narrowing helpers. Faster than `dynamic_pointer_cast<T>`
// for two reasons: (1) single virtual call to type() instead of RTTI
// walk, and (2) static_pointer_cast adjusts the share-count atomically
// without consulting the type-info chain. Use anywhere the save and
// parse engines need to narrow a base ValuePtr to a concrete subclass.
// Returns nullptr for type mismatch or null input.
inline std::shared_ptr<StringValue> as_string(const ValuePtr& v) {
    return (v && v->type() == ValueType::String)
        ? std::static_pointer_cast<StringValue>(v) : nullptr;
}
inline std::shared_ptr<ArrayValue> as_array(const ValuePtr& v) {
    return (v && v->type() == ValueType::Array)
        ? std::static_pointer_cast<ArrayValue>(v) : nullptr;
}
inline std::shared_ptr<DictValue> as_dict(const ValuePtr& v) {
    return (v && v->type() == ValueType::Dict)
        ? std::static_pointer_cast<DictValue>(v) : nullptr;
}
inline std::shared_ptr<IntValue> as_int(const ValuePtr& v) {
    return (v && v->type() == ValueType::Int)
        ? std::static_pointer_cast<IntValue>(v) : nullptr;
}

// Raw-pointer narrowing helpers — the borrowed (non-owning) counterparts
// of the ValuePtr overloads above. Used by read-only engine paths (save
// dispatch) that traverse an AST owned by the caller: no shared_ptr
// copies, no atomic refcount traffic.
inline const StringValue* as_string(const Value* v) {
    return (v && v->type() == ValueType::String)
        ? static_cast<const StringValue*>(v) : nullptr;
}
inline const ArrayValue* as_array(const Value* v) {
    return (v && v->type() == ValueType::Array)
        ? static_cast<const ArrayValue*>(v) : nullptr;
}
inline const DictValue* as_dict(const Value* v) {
    return (v && v->type() == ValueType::Dict)
        ? static_cast<const DictValue*>(v) : nullptr;
}
inline const IntValue* as_int(const Value* v) {
    return (v && v->type() == ValueType::Int)
        ? static_cast<const IntValue*>(v) : nullptr;
}

// Structural deep equality (type + payload; arrays elementwise; dicts
// key-ordered). Order-independent for dicts: FlatDict keeps entries sorted
// by key, so two equal dicts iterate in the same order regardless of
// insertion order.
bool value_equal(const ValuePtr& a, const ValuePtr& b);

} // namespace rawast
