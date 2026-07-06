// Example: build your own value representation on the rawast seam.
//
// rawast is a generic engine. A "representation" is a {Builder, Accessor}
// pair: Builder is the WRITE half (the parser emits typed events into it),
// Accessor is the READ half (the save engine / convert read through it).
// Implement both and you get parse_into / save_from / convert for free —
// the engine core never sees your concrete type.
//
// This file implements a COMPACT, INTERNED representation entirely against
// rawast's PUBLIC headers — no engine internals. It is the shape a
// memory-tight consumer (e.g. an arena AST) would build: all nodes live in
// one flat vector, and every dict key + string value is interned into a
// pool (in real ASTs keys like "type"/"name" and tags like "logic" repeat
// millions of times). Concrete representations like this belong in the
// CONSUMING project, not in the rawast core.
//
// Usage:  custom-representation <grammar.(rawast|json)> <input-file>
// It parses the input into the compact store, saves it back out through the
// compact Accessor, and checks the bytes match the reference representation
// (and the original input) — proving the custom representation round-trips.

#include <rawast/accessor.hpp>
#include <rawast/builder.hpp>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace rawast;

// ---------------------------------------------------------------------------
// The storage: a flat, interned node arena.
// ---------------------------------------------------------------------------
struct CompactStore {
    enum class K : std::uint8_t { Null, Bool, Int, UInt, Real, Str, Arr, Dict };
    struct Node {
        K kind = K::Null;
        std::int64_t  ival = 0;   // Int / Bool
        std::uint64_t uval = 0;   // UInt
        double        rval = 0;   // Real
        std::uint32_t sid  = 0;   // Str -> pool id
        std::vector<std::uint32_t> kids;  // Arr/Dict child node ids
        std::vector<std::uint32_t> keys;  // Dict: interned key ids (|| kids)
    };
    std::vector<Node>        nodes;   // append-only during a parse
    std::vector<std::string> pool;    // interned strings (keys + string values)
    std::unordered_map<std::string, std::uint32_t> intern_map;
    std::uint32_t root = 0;

    std::uint32_t intern(std::string_view s) {
        std::string key(s);
        auto it = intern_map.find(key);
        if (it != intern_map.end()) return it->second;
        std::uint32_t id = static_cast<std::uint32_t>(pool.size());
        pool.push_back(key);
        intern_map.emplace(std::move(key), id);
        return id;
    }
    std::uint32_t add(Node n) {
        nodes.push_back(std::move(n));
        return static_cast<std::uint32_t>(nodes.size() - 1);
    }
};

// ---------------------------------------------------------------------------
// Builder (write half). Uses the "buffer children per open level" pattern,
// so backtracking is trivially correct: checkpoint/rollback/record/replay
// touch only the current level's child list. Leaf/closed-container node ids
// point into the append-only `nodes` vector, which is never truncated —
// a rejected branch leaves orphan nodes (harmless; unreachable from root).
// ---------------------------------------------------------------------------
class CompactBuilder final : public Builder {
public:
    explicit CompactBuilder(CompactStore& s) : s_(s) {
        levels_.push_back({Container::None, {}});
    }

    // A leaf is emitted as a node id; a name-marker string is emitted as an
    // interned KEY id. `is_name` on the EV distinguishes the two, and — as
    // in the reference builder — key/value pairing is deferred to the
    // enclosing dict's end(), so keys that arrive through spliced None
    // sequences (e.g. JSON's `MEMBER`) still land on the right dict.
    void null_(bool n)                override { emit(mk(CompactStore::K::Null), n); }
    void bool_(bool v, bool n)        override { auto x = mk(CompactStore::K::Bool); s_.nodes[x].ival = v; emit(x, n); }
    void int_(std::int64_t v, bool n) override { auto x = mk(CompactStore::K::Int);  s_.nodes[x].ival = v; emit(x, n); }
    void uint_(std::uint64_t v, bool n)override{ auto x = mk(CompactStore::K::UInt); s_.nodes[x].uval = v; emit(x, n); }
    void real_(double v, bool n)      override { auto x = mk(CompactStore::K::Real); s_.nodes[x].rval = v; emit(x, n); }
    void string_(std::string_view v, bool n) override {
        if (n) { levels_.back().evs.push_back({s_.intern(v), true}); return; }  // key marker
        auto x = mk(CompactStore::K::Str); s_.nodes[x].sid = s_.intern(v);
        emit(x, false);
    }

    void begin(Container kind) override { levels_.push_back({kind, {}}); }
    void end() override {
        Level lvl = std::move(levels_.back());
        levels_.pop_back();
        auto& parent = levels_.back().evs;
        switch (lvl.kind) {
        case Container::None:                          // splice, flags intact
            for (auto& e : lvl.evs) parent.push_back(e);
            return;
        case Container::Array: {
            CompactStore::Node n; n.kind = CompactStore::K::Arr;
            for (auto& e : lvl.evs) if (!e.is_name) n.kids.push_back(e.v);
            emit(s_.add(std::move(n)), false);
            return;
        }
        case Container::Dict: {
            CompactStore::Node n; n.kind = CompactStore::K::Dict;
            std::uint32_t key = 0; bool have = false;
            for (auto& e : lvl.evs) {
                if (e.is_name) { key = e.v; have = true; }
                else if (have)  { n.keys.push_back(key); n.kids.push_back(e.v); have = false; }
            }
            emit(s_.add(std::move(n)), false);
            return;
        }
        }
    }

    Checkpoint checkpoint() const override {
        return {levels_.size(), levels_.back().evs.size()};
    }
    void rollback(Checkpoint cp) override {
        while (levels_.size() > cp.depth) levels_.pop_back();
        auto& evs = levels_.back().evs;
        if (cp.size <= evs.size()) evs.resize(cp.size);
    }
    Recording record_from(Checkpoint cp) const override {
        const auto& evs = levels_[cp.depth - 1].evs;
        return std::make_shared<std::vector<EV>>(evs.begin() + cp.size, evs.end());
    }
    void replay(const Recording& rec) override {
        const auto& evs = *static_cast<const std::vector<EV>*>(rec.get());
        auto& cur = levels_.back().evs;
        cur.insert(cur.end(), evs.begin(), evs.end());
    }

    void finish() {
        for (auto& e : levels_[0].evs) if (!e.is_name) { s_.root = e.v; break; }
    }

private:
    struct EV { std::uint32_t v; bool is_name; };   // v = node id, or key id if is_name
    struct Level { Container kind; std::vector<EV> evs; };

    std::uint32_t mk(CompactStore::K k) {
        CompactStore::Node n; n.kind = k; return s_.add(std::move(n));
    }
    void emit(std::uint32_t node_id, bool is_name) {
        levels_.back().evs.push_back({node_id, is_name});
    }

    CompactStore&      s_;
    std::vector<Level> levels_;
};

// ---------------------------------------------------------------------------
// Accessor (read half). Node == a pointer to a CompactStore::Node (the
// arena is stable once the parse finishes). each() yields keys SORTED, as
// the Accessor contract requires.
// ---------------------------------------------------------------------------
class CompactAccessor final : public Accessor {
public:
    explicit CompactAccessor(const CompactStore& s) : s_(s) {}

    Node root() const override { return &s_.nodes[s_.root]; }
    ValueType kind(Node n) const override {
        switch (nd(n)->kind) {
        case CompactStore::K::Null: return ValueType::Null;
        case CompactStore::K::Bool: return ValueType::Bool;
        case CompactStore::K::Int:  return ValueType::Int;
        case CompactStore::K::UInt: return ValueType::UInt;
        case CompactStore::K::Real: return ValueType::Real;
        case CompactStore::K::Str:  return ValueType::String;
        case CompactStore::K::Arr:  return ValueType::Array;
        case CompactStore::K::Dict: return ValueType::Dict;
        }
        return ValueType::Null;
    }
    bool             bool_(Node n)  const override { return nd(n)->ival != 0; }
    std::int64_t     int_(Node n)   const override { return nd(n)->ival; }
    std::uint64_t    uint_(Node n)  const override { return nd(n)->uval; }
    double           real_(Node n)  const override { return nd(n)->rval; }
    std::string_view string_(Node n)const override { return s_.pool[nd(n)->sid]; }

    std::size_t size(Node n) const override { return nd(n)->kids.size(); }
    Node at(Node n, std::size_t i) const override { return &s_.nodes[nd(n)->kids[i]]; }
    Node get(Node n, std::string_view name) const override {
        const auto* d = nd(n);
        for (std::size_t i = 0; i < d->kids.size(); ++i)
            if (s_.pool[d->keys[i]] == name) return &s_.nodes[d->kids[i]];
        return nullptr;
    }
    void each(Node n,
              const std::function<void(std::string_view, Node)>& fn) const override {
        const auto* d = nd(n);
        std::vector<std::size_t> order(d->kids.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return s_.pool[d->keys[a]] < s_.pool[d->keys[b]];
        });
        for (std::size_t i : order)
            fn(s_.pool[d->keys[i]], &s_.nodes[d->kids[i]]);
    }

private:
    const CompactStore::Node* nd(Node n) const {
        return static_cast<const CompactStore::Node*>(n);
    }
    const CompactStore& s_;
};

// ---------------------------------------------------------------------------
static std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <grammar.(rawast|json)> <input>\n", argv[0]);
        return 2;
    }
    register_std_parser_group();
    Grammar g;
    const std::string gpath = argv[1];
    auto lr = (gpath.size() > 7 && gpath.substr(gpath.size() - 7) == ".rawast")
        ? load_rawast_grammar_from_file(g, gpath)
        : load_json_grammar_from_file(g, gpath);
    if (!lr) { std::fprintf(stderr, "grammar load failed: %s\n", lr.error().c_str()); return 1; }

    const std::string input = slurp(argv[2]);

    // Parse the input INTO the custom compact representation.
    CompactStore store;
    CompactBuilder cb(store);
    {
        auto stream = Stream::from_string(input);
        auto r = g.parse_into(stream, cb);
        if (!r) { std::fprintf(stderr, "parse failed: %s\n", r.error().message.c_str()); return 1; }
        cb.finish();
    }

    // Save it back OUT through the custom Accessor.
    CompactAccessor ca(store);
    std::ostringstream out_custom;
    if (auto r = g.save_from(out_custom, ca); !r) {
        std::fprintf(stderr, "save_from(custom) failed: %s\n", r.error().message.c_str());
        return 1;
    }

    // Reference: parse+save through rawast's own shared_ptr representation.
    SharedPtrBuilder rb;
    {
        auto stream = Stream::from_string(input);
        auto r = g.parse_into(stream, rb);
        if (!r) { std::fprintf(stderr, "reference parse failed\n"); return 1; }
    }
    SharedPtrAccessor ra(rb.result());
    std::ostringstream out_ref;
    (void)g.save_from(out_ref, ra);

    const bool matches_ref   = out_custom.str() == out_ref.str();
    const bool round_trips   = out_custom.str() == input;
    std::printf("nodes=%zu  interned_strings=%zu\n", store.nodes.size(), store.pool.size());
    std::printf("custom-save == reference-save : %s\n", matches_ref ? "yes" : "NO");
    std::printf("custom-save == original input : %s\n", round_trips ? "yes" : "(grammar not byte-exact)");
    return matches_ref ? 0 : 1;
}
