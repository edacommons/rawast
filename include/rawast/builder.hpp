#pragma once

#include <rawast/node.hpp>     // Container
#include <rawast/value.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rawast {

class ValuePool;

// Generic construction sink — the write half of a representation pair.
// The parse driver emits these events as it walks a grammar; a Builder
// materialises whatever representation it wants. The engine core never
// depends on a concrete value type: plug-ins implement the TYPED leaf
// events plus begin/end/checkpoint/rollback/record/replay, and never see
// a ValuePtr.
//
// Event model:
//   null_/bool_/int_/uint_/real_/string_ — a leaf, with `is_name` flagging
//                        a dict key / name marker (meaningful for strings;
//                        carried uniformly for interface regularity)
//   begin(Array|Dict)  — open a container (None: children flow through to
//                        the enclosing container)
//   end()              — materialise the open container into the parent
//   checkpoint/rollback — PEG backtracking: discard everything emitted
//                        since the checkpoint (a rejected alternative)
//   record_from/replay — sequence-success cache: capture what a completed
//                        subtree contributed, re-emit it on a cache hit.
//                        The Recording is builder-defined (opaque).
//   adopt(ValuePtr)    — hand over a completed reference-model subtree
//                        (subparse product, scope capture, grammar
//                        constant). Default implementation translates the
//                        tree into typed events, so plug-ins get it for
//                        free; Value-based builders may override for
//                        zero-copy adoption.
class Builder {
public:
    virtual ~Builder() = default;

    struct Checkpoint {
        std::size_t depth;   // number of open levels
        std::size_t size;    // emitted count at the top level
    };

    // Opaque, builder-defined snapshot of a completed subtree's
    // contribution to its parent level.
    using Recording = std::shared_ptr<const void>;

    // --- typed leaf events (the plug-in surface) ---
    virtual void null_(bool is_name) = 0;
    virtual void bool_(bool v, bool is_name) = 0;
    virtual void int_(std::int64_t v, bool is_name) = 0;
    virtual void uint_(std::uint64_t v, bool is_name) = 0;
    virtual void real_(double v, bool is_name) = 0;
    virtual void string_(std::string_view v, bool is_name) = 0;

    // --- containers ---
    virtual void begin(Container kind) = 0;
    virtual void end() = 0;

    // --- backtracking ---
    virtual Checkpoint checkpoint() const = 0;
    virtual void       rollback(Checkpoint) = 0;

    // --- cache ---
    virtual Recording record_from(Checkpoint cp) const = 0;
    virtual void      replay(const Recording&) = 0;

    // --- reference-model adoption (default: translate to typed events) ---
    virtual void adopt(const ValuePtr& v, bool is_name);
};

// The reference builder: materialises the classic shared_ptr<Value> AST.
// Owns the interning policy (identical primitives share one canonical
// ValuePtr via the pool) — a representation concern, not the engine's.
class SharedPtrBuilder final : public Builder {
public:
    explicit SharedPtrBuilder(ValuePool& pool);
    // Self-contained form: owns its interning pool. The representation-
    // bundle sugar (parse_as<SharedPtrRepr>) constructs builders this way.
    SharedPtrBuilder();

    void null_(bool is_name) override;
    void bool_(bool v, bool is_name) override;
    void int_(std::int64_t v, bool is_name) override;
    void uint_(std::uint64_t v, bool is_name) override;
    void real_(double v, bool is_name) override;
    void string_(std::string_view v, bool is_name) override;

    void begin(Container kind) override;
    void end() override;
    Checkpoint checkpoint() const override;
    void       rollback(Checkpoint) override;
    Recording  record_from(Checkpoint cp) const override;
    void       replay(const Recording&) override;

    // Zero-copy adoption: scalars are interned, composites are shared.
    void adopt(const ValuePtr& v, bool is_name) override;

    // The single accumulated top-level value (after the parse completes).
    ValuePtr result() const;

    // Return to the just-constructed state (only the root level, empty)
    // WITHOUT freeing the levels_ buffer — retains capacity so a caller
    // that reuses one builder across many parses (the scan-driven
    // preprocessor, once per byte) pays no per-parse allocation.
    void reset();

    // Open-level count (1 == only the root; >1 means unclosed begins).
    std::size_t depth() const { return levels_.size(); }

private:
    struct EmittedValue { ValuePtr value; bool is_name; };
    struct Level { Container kind; std::vector<EmittedValue> emitted; };

    void push(ValuePtr v, bool is_name);

    std::unique_ptr<ValuePool> owned_pool_;   // set only by the default ctor
    ValuePool&         pool_;
    std::vector<Level> levels_;   // levels_[0] is the root (Container::None)
};

} // namespace rawast
