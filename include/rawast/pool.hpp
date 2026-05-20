#pragma once

#include <rawast/value.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace rawast {

// Primitive-value interning pool with container back-references.
//
// Two responsibilities:
//
//   1. Interning. `intern(v)` returns the canonical ValuePtr for a value
//      with the same content; the first call seeds the entry, subsequent
//      calls with equal content return the same instance. Null/Bool are
//      already singletons and are passed through unchanged.
//
//   2. Container back-references. `register_usage(value, container)`
//      records that `value` appears as a child of `container`. The
//      reverse-index `find_containers_of(value)` returns the list of
//      containers that hold the value — the substrate for value search
//      (§3.5 / M2 of the proposal: "find all positions of net 'clk'").
//
// Pool lifetime is per-parse. The pool fills during a parse and is
// owned by the caller after parse() returns; the canonical Value
// instances inside the parsed tree are stable as long as the pool
// keeps them alive.
class ValuePool {
public:
    ValuePool() = default;

    ValuePool(const ValuePool&) = delete;
    ValuePool& operator=(const ValuePool&) = delete;
    ValuePool(ValuePool&&) = default;
    ValuePool& operator=(ValuePool&&) = default;

    // Intern a primitive value. Composite values (Array, Dict) are passed
    // through unchanged; subtree hash-consing is a deferred opt-in feature.
    ValuePtr intern(ValuePtr v);

    // Convenience: intern by typed payload.
    ValuePtr intern_int(std::int64_t v);
    ValuePtr intern_uint(std::uint64_t v);
    ValuePtr intern_real(double v);
    ValuePtr intern_string(std::string v);

    // Record that `value` appears as a child of `container`. The same
    // (value, container) pair may be registered multiple times if the
    // value appears at multiple positions within the same container.
    void register_usage(ValuePtr value, ValuePtr container);

    // Get the list of containers (parents) that hold this canonical value.
    // Returns an empty vector if no usages were registered.
    std::vector<ValuePtr> find_containers_of(const ValuePtr& value) const;

    // Number of unique interned primitive entries (Null/Bool not counted).
    std::size_t size() const noexcept;

private:
    std::unordered_map<std::int64_t, ValuePtr> ints_;
    std::unordered_map<std::uint64_t, ValuePtr> uints_;
    std::map<double, ValuePtr> reals_;
    std::unordered_map<std::string, ValuePtr> strings_;

    // Back-references keyed by raw Value pointer (identity).
    std::unordered_map<const Value*, std::vector<ValuePtr>> back_refs_;
};

} // namespace rawast
