#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rawast {

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

class Value;
using ValuePtr = std::shared_ptr<Value>;

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
    std::map<std::string, ValuePtr> data_;
public:
    DictValue() = default;
    ValueType type() const noexcept override { return ValueType::Dict; }
    const std::map<std::string, ValuePtr>& data() const noexcept { return data_; }
    std::map<std::string, ValuePtr>& data() noexcept { return data_; }
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
// key-ordered). Order-independent for dicts since DictValue is a std::map.
bool value_equal(const ValuePtr& a, const ValuePtr& b);

} // namespace rawast
