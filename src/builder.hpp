#pragma once

#include <rawast/node.hpp>     // Container
#include <rawast/value.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace rawast {

class ValuePool;

// Generic construction sink. The parse driver emits these events as it walks a
// grammar; a Builder materialises whatever representation it wants — the parser
// stops knowing what an AST *is*. This is the seam that lets one driver target
// the classic shared_ptr<Value> tree, a future arena, native Python, or a host
// structure (e.g. dagland's L0), all behind the same interface.
//
// Event model (mirrors the current Frame: emitted_ accumulator + finish()):
//   value(v, is_name)  — a leaf; is_name flags a dict key / name marker
//   begin(Array|Dict)  — open a container (None never opens; its children flow
//                        to the enclosing container)
//   end()              — materialise the open container, hand it to the parent
//   checkpoint/rollback — PEG backtracking: discard everything emitted since the
//                        checkpoint (a rejected alternative)
//
// NOTE: this is the standalone interface + default builder. Wiring it into the
// parse driver (replacing the inline Frame logic, and delegating the
// sequence-success cache's snapshot/replay) is the separate, corpus-gated step.
class Builder {
public:
    virtual ~Builder() = default;

    struct Checkpoint {
        std::size_t depth;   // number of open levels
        std::size_t size;    // emitted count at the top level
    };

    // A snapshot of the values a completed subtree contributed to its parent —
    // used by the sequence-success cache to replay a frame's emissions on a hit
    // instead of re-parsing. ValuePtr-based for now (SharedPtrBuilder); a future
    // arena builder would make Recording opaque/builder-defined.
    struct EmittedValue { ValuePtr value; bool is_name; };
    using Recording = std::vector<EmittedValue>;

    virtual void value(ValuePtr v, bool is_name) = 0;
    virtual void begin(Container kind) = 0;
    virtual void end() = 0;

    virtual Checkpoint checkpoint() const = 0;
    virtual void       rollback(Checkpoint) = 0;

    // Cache support: capture what was emitted to `cp`'s level since `cp`, and
    // replay it into the current level (the cache-hit path).
    virtual Recording record_from(Checkpoint cp) const = 0;
    virtual void       replay(const Recording&) = 0;
};

// The default builder: reproduces today's shared_ptr<Value> AST exactly. The
// materialisation logic is lifted verbatim from Frame::finish (array/dict
// assembly, the `name[]` list-append marker, and ValuePool back-references).
class SharedPtrBuilder final : public Builder {
public:
    explicit SharedPtrBuilder(ValuePool& pool);

    void value(ValuePtr v, bool is_name) override;
    void begin(Container kind) override;
    void end() override;
    Checkpoint checkpoint() const override;
    void       rollback(Checkpoint) override;
    Recording  record_from(Checkpoint cp) const override;
    void       replay(const Recording&) override;

    // The single accumulated top-level value (after the parse completes).
    ValuePtr result() const;

    // Open-level count (1 == only the root; >1 means unclosed begins).
    std::size_t depth() const { return levels_.size(); }

private:
    struct Level { Container kind; std::vector<EmittedValue> emitted; };

    std::vector<Level> levels_;   // levels_[0] is the root (Container::None)
};

} // namespace rawast
