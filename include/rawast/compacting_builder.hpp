#pragma once

#include <rawast/builder.hpp>
#include <rawast/value.hpp>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rawast {

class Grammar;

// Builder decorator that applies the grammar's `#opchain` compaction IN THE
// EVENT STREAM: always-wrap `{lhs, tail:[{op, rhs}…]}` dicts are folded into
// compact `{op, args}` before reaching the wrapped builder. This makes the
// transform representation-independent — wrap ANY builder (native Python, an
// arena, a host structure) and it receives compacted values, with no detour
// through the reference model.
//
// Mechanics: events are buffered into a small representation-agnostic node
// tree; each dict `end()` assembles its fields (including `name[]` append
// semantics) and applies the same fold the reference wrapper uses (same
// guards: fold-produced-only arg extension, foldable-op restriction from the
// grammar's cascade ladder, non-`rhs` tail bailout, unwrap-to-lhs). The
// wrapped builder sees only committed, folded values — PEG backtracking is
// absorbed entirely by the buffer. `adopt` payloads get the reference fold
// applied before forwarding (parity: the classic whole-tree pass folded
// inside adopted subtrees too).
//
// A Builder has no end-of-parse event, so the owner calls finish() after a
// successful parse to flush the buffered root into the wrapped builder.
//
// Only useful for grammars with `#opchain` (Grammar::has_any_opchain());
// for others it is pure overhead — don't wrap.
class CompactingBuilder final : public Builder {
public:
    CompactingBuilder(Builder& inner, const Grammar& g);

    void null_(bool is_name) override;
    void bool_(bool v, bool is_name) override;
    void int_(std::int64_t v, bool is_name) override;
    void uint_(std::uint64_t v, bool is_name) override;
    void real_(double v, bool is_name) override;
    void string_(std::string_view v, bool is_name) override;
    void adopt(const ValuePtr& v, bool is_name) override;

    void begin(Container kind) override;
    void end() override;

    Checkpoint checkpoint() const override;
    void       rollback(Checkpoint) override;
    Recording  record_from(Checkpoint cp) const override;
    void       replay(const Recording&) override;

    // Flush the buffered, folded root value(s) into the wrapped builder.
    void finish();

private:
    struct Node {
        enum class K { Null, Bool, Int, UInt, Real, Str, Adopted, Arr, Dict };
        K k = K::Null;
        bool          b = false;
        std::int64_t  i = 0;
        std::uint64_t u = 0;
        double        r = 0;
        std::string   s;
        ValuePtr      adopted;
        std::vector<Node> items;                             // Arr
        std::vector<std::pair<std::string, Node>> fields;    // Dict
    };
    struct Level { Container kind; std::vector<std::pair<Node, bool>> emitted; };

    void push(Node n, bool is_name);
    Node assemble_dict(std::vector<std::pair<Node, bool>>&& emitted) const;
    Node fold(Node dict) const;
    void replay_node(const Node& n, bool is_name);

    Builder&                        inner_;
    std::unordered_set<std::string> ops_;
    bool                            restrict_ops_ = false;
    std::vector<Level>              levels_;
};

} // namespace rawast
