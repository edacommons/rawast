#pragma once

#include <rawast/value.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace rawast {

class Builder;

// Generic read interface — the read half of a representation pair, and the
// save engine's view of an AST. A representation implements Accessor so
// `Grammar::save_from` can serialise it and so generic transforms
// (`convert`, equality) can traverse it. The engine core never depends on
// a concrete value type.
//
// Node is an opaque per-representation cookie (a borrowed pointer/id valid
// while the accessor's document is alive). kind() classifies it; the typed
// getters are defined only for the matching kind. Dict iteration order is
// the representation's canonical order and MUST be stable and key-sorted —
// round-trip equality and save dispatch rely on it.
class Accessor {
public:
    using Node = const void*;

    virtual ~Accessor() = default;

    virtual Node root() const = 0;
    virtual ValueType kind(Node) const = 0;

    // --- scalars (defined only for the matching kind) ---
    virtual bool             bool_(Node) const = 0;
    virtual std::int64_t     int_(Node) const = 0;
    virtual std::uint64_t    uint_(Node) const = 0;
    virtual double           real_(Node) const = 0;
    virtual std::string_view string_(Node) const = 0;

    // --- containers ---
    virtual std::size_t size(Node) const = 0;                 // entries
    virtual Node        at(Node, std::size_t) const = 0;      // array element
    virtual Node        get(Node, std::string_view) const = 0; // dict field; nullptr if absent
    virtual void        each(Node,
        const std::function<void(std::string_view, Node)>&) const = 0; // dict, key-sorted
};

// The reference accessor: reads the classic shared_ptr<Value> AST.
// Node == const Value*.
class SharedPtrAccessor final : public Accessor {
public:
    explicit SharedPtrAccessor(ValuePtr root) : root_(std::move(root)) {}

    const ValuePtr& root_value() const { return root_; }

    Node root() const override { return root_.get(); }
    ValueType kind(Node n) const override {
        return static_cast<const Value*>(n)->type();
    }
    bool bool_(Node n) const override {
        return static_cast<const BoolValue*>(n)->data();
    }
    std::int64_t int_(Node n) const override {
        return static_cast<const IntValue*>(n)->data();
    }
    std::uint64_t uint_(Node n) const override {
        return static_cast<const UIntValue*>(n)->data();
    }
    double real_(Node n) const override {
        return static_cast<const RealValue*>(n)->data();
    }
    std::string_view string_(Node n) const override {
        return static_cast<const StringValue*>(n)->data();
    }
    std::size_t size(Node n) const override {
        const Value* v = static_cast<const Value*>(n);
        if (v->type() == ValueType::Array)
            return static_cast<const ArrayValue*>(v)->data().size();
        if (v->type() == ValueType::Dict)
            return static_cast<const DictValue*>(v)->data().size();
        return 0;
    }
    Node at(Node n, std::size_t i) const override {
        return static_cast<const ArrayValue*>(n)->data()[i].get();
    }
    Node get(Node n, std::string_view name) const override {
        const auto& m = static_cast<const DictValue*>(n)->data();
        auto it = m.find(std::string(name));
        return it == m.end() ? nullptr : it->second.get();
    }
    void each(Node n,
              const std::function<void(std::string_view, Node)>& fn) const override {
        for (const auto& [k, v] : static_cast<const DictValue*>(n)->data())
            fn(k, v.get());
    }

private:
    ValuePtr root_;
};

// The generic representation pipe: read `n` (default: the root) through
// accessor `a` and emit it as typed events into builder `b`. This is the
// universal transform/conversion primitive — parse into representation A,
// convert to representation B, save from either.
void convert(const Accessor& a, Accessor::Node n, Builder& b,
             bool is_name = false);
void convert(const Accessor& a, Builder& b);

} // namespace rawast
