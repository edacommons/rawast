#include <rawast/grammar.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_registry.hpp>
#include <rawast/scan_context.hpp>

#include "save_stack.hpp"

#include "frame.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <unordered_set>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rawast {

// -------------------------------------------------------------------------
// Node allocation and builders
// -------------------------------------------------------------------------

NodeId Grammar::allocate_(NodeKind kind) {
    NodeId id{nodes_.size()};
    nodes_.emplace_back();
    nodes_.back().kind = kind;
    return id;
}

NodeId Grammar::new_choice()   { return allocate_(NodeKind::Choice); }
NodeId Grammar::new_sequence() { return allocate_(NodeKind::Sequence); }
NodeId Grammar::new_repeat()   { return allocate_(NodeKind::Repeat); }
NodeId Grammar::new_raw()      { return allocate_(NodeKind::Raw); }

NodeId Grammar::new_ref(std::string name) {
    NodeId id = allocate_(NodeKind::Ref);
    nodes_[id.value()].value = make_string(std::move(name));
    return id;
}

NodeId Grammar::new_key(std::string token) {
    NodeId id = allocate_(NodeKind::Key);
    nodes_[id.value()].value = make_string(std::move(token));
    return id;
}

NodeId Grammar::new_parse(std::string parser_name) {
    NodeId id = allocate_(NodeKind::Parse);
    nodes_[id.value()].value = make_string(std::move(parser_name));
    return id;
}

NodeId Grammar::new_value(ValuePtr v) {
    NodeId id = allocate_(NodeKind::Value);
    nodes_[id.value()].value = std::move(v);
    return id;
}

NodeId Grammar::add_ref(NodeId parent, std::string name) {
    NodeId child = new_ref(std::move(name));
    nodes_[parent.value()].children.push_back(child);
    return child;
}

NodeId Grammar::add_key(NodeId parent, std::string token) {
    NodeId child = new_key(std::move(token));
    nodes_[parent.value()].children.push_back(child);
    return child;
}

NodeId Grammar::add_parse(NodeId parent, std::string parser_name) {
    NodeId child = new_parse(std::move(parser_name));
    nodes_[parent.value()].children.push_back(child);
    return child;
}

NodeId Grammar::add_value(NodeId parent, ValuePtr v) {
    NodeId child = new_value(std::move(v));
    nodes_[parent.value()].children.push_back(child);
    return child;
}

NodeId Grammar::add_choice(NodeId parent) {
    NodeId child = new_choice();
    nodes_[parent.value()].children.push_back(child);
    return child;
}

NodeId Grammar::add_sequence(NodeId parent) {
    NodeId child = new_sequence();
    nodes_[parent.value()].children.push_back(child);
    return child;
}

NodeId Grammar::add_repeat(NodeId parent) {
    NodeId child = new_repeat();
    nodes_[parent.value()].children.push_back(child);
    return child;
}

void Grammar::set_optional(NodeId id) {
    nodes_[id.value()].is_optional = true;
}

void Grammar::set_negative(NodeId id) {
    nodes_[id.value()].is_negative = true;
}

void Grammar::set_name(NodeId id) {
    nodes_[id.value()].is_name = true;
}

void Grammar::set_container(NodeId id, Container c) {
    nodes_[id.value()].container = c;
}

// --- First-byte precomputation ------------------------------------------
//
// For each node, compute the set of input first-bytes its first
// non-nullable terminal can begin with. Used by parse_from to skip
// optional / choice-alt frame pushes when the input first byte
// can't match. Recursive; we memoize during the walk.
namespace {

struct FirstByteResult {
    std::bitset<256> bytes;
    bool nullable = false;
    bool known    = true;  // false ⇒ "any byte possible, fall back"
};

// Mark "every byte possible" — used for Parse nodes whose set
// we don't try to derive analytically.
inline FirstByteResult any_byte_result() {
    FirstByteResult r;
    r.bytes.set();
    r.nullable = false;
    r.known    = false;
    return r;
}

FirstByteResult compute_node_first_bytes(
        const Grammar& g, NodeId id,
        std::vector<int>& state, std::vector<FirstByteResult>& cache) {
    if (!id.valid()) {
        FirstByteResult r;
        r.nullable = true;
        return r;
    }
    std::size_t i = id.value();
    if (state[i] == 2) return cache[i];
    if (state[i] == 1) {
        // Cycle — treat as "unknown" so the parse driver falls back.
        return any_byte_result();
    }
    state[i] = 1;

    FirstByteResult result;
    // Track the ORIGINAL node's is_optional flag — for a Ref like
    // `?<RULE>`, the optional sits on the Ref, not on RULE's body.
    // An optional Ref is nullable: when the input's next byte
    // isn't in its first-byte set, the optional can SKIP, and the
    // sequence continues to the NEXT child. Without this, a rule
    // like `?<VIS>, ?<LIFE>, <TYPE>` would have a first-byte set
    // limited to VIS's (l, p) because VIS isn't itself nullable —
    // the engine's peek-and-skip would then wrongly exclude inputs
    // starting with TYPE keywords (i, w, r, ...).
    const Node& orig = g.node(id);
    const bool orig_optional = orig.is_optional;
    NodeId resolved = g.resolve_ref(id);
    const Node& n = g.node(resolved);

    switch (n.kind) {
    case NodeKind::Value:
        // Doesn't consume input.
        result.nullable = true;
        break;
    case NodeKind::Key: {
        // Single literal token. First byte is the first char of the
        // key (if non-empty). Otherwise nullable.
        if (auto sv = std::dynamic_pointer_cast<StringValue>(n.value)) {
            if (!sv->data().empty()) {
                result.bytes.set(static_cast<unsigned char>(sv->data().front()));
                result.known = true;
            } else {
                result.nullable = true;
            }
        } else {
            result = any_byte_result();
        }
        break;
    }
    case NodeKind::Parse: {
        // Ask the parser itself for its first-byte set via the
        // Parser::first_bytes() spec string. Each shipped parser
        // overrides this with a compile-time-concatenated spec built
        // from the INC_RANGE/EXC_RANGE/INC_CHAR/EXC_CHAR/ALL_BYTES
        // macros in parser.hpp. Default (unknown / user-defined
        // parsers that don't override) returns an empty spec, which
        // we treat as the conservative "any byte" fallback — matches
        // the legacy behaviour for parsers without a known first-byte
        // set.
        std::string pname;
        if (auto sv = std::dynamic_pointer_cast<StringValue>(n.value)) {
            pname = sv->data();
        }
        Parser* p = g.parser(pname);
        std::string_view spec = p ? p->first_bytes() : std::string_view{};
        if (spec.empty()) {
            result = any_byte_result();
        } else {
            // Decode the spec: consecutive 3-byte entries
            //   byte[0] = '+' include or '-' exclude
            //   byte[1] = lo (range low, inclusive)
            //   byte[2] = hi (range high, inclusive)
            for (std::size_t i = 0; i + 2 < spec.size(); i += 3) {
                char op = spec[i];
                unsigned lo = static_cast<unsigned char>(spec[i + 1]);
                unsigned hi = static_cast<unsigned char>(spec[i + 2]);
                if (op == '+') {
                    for (unsigned c = lo; c <= hi; ++c) result.bytes.set(c);
                } else if (op == '-') {
                    for (unsigned c = lo; c <= hi; ++c) result.bytes.reset(c);
                }
            }
            result.known = true;
        }
        break;
    }
    case NodeKind::Raw:
        // Raw consumes anything until a stop literal; first byte
        // unconstrained.
        result = any_byte_result();
        break;
    case NodeKind::Scope: {
        // Scope has no opening delimiter of its own (the surrounding
        // sequence's previous sibling supplies any start literal);
        // first byte is unconstrained, same as Raw.
        result = any_byte_result();
        result.nullable = false;
        break;
    }
    case NodeKind::Choice: {
        // Union of children's first-byte sets. Nullable iff any
        // alternative is nullable.
        for (NodeId c : n.children) {
            auto sub = compute_node_first_bytes(g, c, state, cache);
            if (!sub.known) {
                result = any_byte_result();
                break;
            }
            result.bytes |= sub.bytes;
            if (sub.nullable) result.nullable = true;
        }
        break;
    }
    case NodeKind::Sequence: {
        // Walk children until we hit a non-nullable one. Children
        // before that contribute their first-bytes to the union.
        // If all children nullable, the sequence is nullable.
        result.nullable = true;
        bool ok = true;
        for (NodeId c : n.children) {
            const Node& cn = g.node(g.resolve_ref(c));
            if (cn.kind == NodeKind::Value) continue;
            auto sub = compute_node_first_bytes(g, c, state, cache);
            if (!sub.known) {
                result = any_byte_result();
                ok = false;
                break;
            }
            result.bytes |= sub.bytes;
            if (!sub.nullable) {
                result.nullable = false;
                break;
            }
        }
        if (!ok) break;
        break;
    }
    case NodeKind::Repeat: {
        // `repeat <X>` is nullable (0 iterations possible) — its
        // first-byte set is X's first-byte set.
        result.nullable = true;
        const std::size_t item_idx = n.has_separator ? 1 : 0;
        if (item_idx < n.children.size()) {
            auto sub = compute_node_first_bytes(
                g, n.children[item_idx], state, cache);
            if (!sub.known) {
                result = any_byte_result();
                result.nullable = true;
            } else {
                result.bytes |= sub.bytes;
            }
        }
        break;
    }
    case NodeKind::Ref:
        // Should have been resolved above.
        result = any_byte_result();
        break;
    }

    // (Earlier attempt to fold rule-local ignore parsers' first-byte
    // sets into the rule's predictive set was reverted — it broke
    // `?<X>` adjacency at the parent level when X had `ignore linespace`,
    // because the predictive check would take the optional on a leading
    // space, then run_ignore inside X's body would consume that space,
    // and a subsequent failure inside X would leave the space lost. The
    // optional's mark/restore would only restore to AFTER the eaten
    // whitespace. See conversation log: SV preprocessor's
    // MACRO_USE / MACRO_ARGS adjacency case.)

    // An optional original node is nullable — its first-byte set
    // is the union with "anything that comes after." The Sequence
    // walker that called us uses `nullable` to decide whether to
    // continue accumulating next-child bytes; making this true for
    // any optional fixes the `?<VIS>, ?<LIFE>, <TYPE>` first-byte
    // computation.
    if (orig_optional) result.nullable = true;

    // Negative-lookahead `!X` inverts the byte set: it succeeds (and
    // contributes to the sequence's first-byte set) precisely when
    // the next input byte CANNOT start X. The match consumes zero
    // input, so the lookahead is always nullable from a sequence's
    // perspective — the next sequence child contributes its own
    // first-byte set in addition.
    if (orig.is_negative) {
        if (result.known) {
            result.bytes.flip();
        }
        result.nullable = true;
    }

    state[i] = 2;
    cache[i] = result;
    return result;
}

} // namespace

void Grammar::compute_first_bytes() const {
    if (first_bytes_computed_) return;
    const std::size_t N = nodes_.size();
    first_bytes_.assign(N, std::bitset<256>{});
    strict_first_bytes_.assign(N, std::bitset<256>{});
    first_bytes_known_.assign(N, false);
    is_optional_chain_.assign(N, false);
    std::vector<int>             state(N, 0);
    std::vector<FirstByteResult> cache(N);
    for (std::size_t i = 0; i < N; ++i) {
        if (state[i] != 0) continue;
        compute_node_first_bytes(*this, NodeId{i}, state, cache);
    }
    for (std::size_t i = 0; i < N; ++i) {
        if (state[i] == 2 && cache[i].known) {
            first_bytes_known_[i] = true;
            // Strict content bytes — always the actual computed set,
            // regardless of nullability. should_skip_optional uses
            // this to skip an `?<X>` whose body can't start at the
            // next byte (skipping is equivalent to matching empty).
            strict_first_bytes_[i] = cache[i].bytes;
            // Nullable nodes: any byte is fine (since "skip" is a
            // valid match). Encode as "known" + all bits set so the
            // parse loop's peek check never wrongly rejects them in
            // a Choice context where a sibling might match.
            if (cache[i].nullable) {
                first_bytes_[i].set();
            } else {
                first_bytes_[i] = cache[i].bytes;
            }
        }
    }
    // Precompute the "is this node in an optional chain?" flag per
    // NodeId. Lets should_skip_optional in parse_from short-circuit
    // in O(1) on the common case (non-optional pushes). A node is in
    // an optional chain if its own is_optional is set, OR it's a Ref
    // and the resolved chain contains an is_optional link.
    for (std::size_t i = 0; i < N; ++i) {
        if (nodes_[i].is_optional) {
            is_optional_chain_[i] = true;
            continue;
        }
        if (nodes_[i].kind != NodeKind::Ref) continue;
        NodeId cur{i};
        bool optional = false;
        while (cur.valid()) {
            const Node& cn = nodes_[cur.value()];
            if (cn.is_optional) { optional = true; break; }
            if (cn.kind != NodeKind::Ref) break;
            auto sv = std::dynamic_pointer_cast<StringValue>(cn.value);
            if (!sv) break;
            if (!has_rule(sv->data())) break;
            NodeId next = rule_id(sv->data());
            if (next.value() == cur.value()) break;
            cur = next;
        }
        is_optional_chain_[i] = optional;
    }
    first_bytes_computed_ = true;
}

void Grammar::set_separator(NodeId parent, NodeId sep) {
    Node& p = nodes_[parent.value()];
    if (p.has_separator && !p.children.empty()) {
        p.children[0] = sep;
    } else {
        p.children.insert(p.children.begin(), sep);
        p.has_separator = true;
    }
}

void Grammar::set_backtrack(NodeId id) {
    nodes_[id.value()].backtrack = true;
}

void Grammar::set_opchain(NodeId id) {
    nodes_[id.value()].opchain = true;
}

bool Grammar::has_opchain(NodeId id) const noexcept {
    if (id.value() >= nodes_.size()) return false;
    return nodes_[id.value()].opchain;
}

bool Grammar::has_any_opchain() const noexcept {
    for (const auto& n : nodes_) {
        if (n.opchain) return true;
    }
    return false;
}

bool Grammar::has_opchain_in_chain(NodeId id) const noexcept {
    NodeId cur = id;
    std::unordered_set<std::uint32_t> seen;
    while (cur.valid() && cur.value() < nodes_.size()
           && seen.insert(cur.value()).second) {
        if (nodes_[cur.value()].opchain) return true;
        if (nodes_[cur.value()].kind != NodeKind::Ref) return false;
        auto sv = std::dynamic_pointer_cast<StringValue>(
            nodes_[cur.value()].value);
        if (!sv) return false;
        auto it = named_rules_.find(sv->data());
        if (it == named_rules_.end()) return false;
        cur = it->second;
    }
    return false;
}

void Grammar::set_fixed_schema(NodeId id) {
    nodes_[id.value()].fixed_schema = true;
}

void Grammar::set_strict(NodeId id) {
    nodes_[id.value()].strict = true;
}

void Grammar::set_min(NodeId id, std::uint32_t m) {
    nodes_[id.value()].min = m;
}

void Grammar::set_indent(NodeId id)  { nodes_[id.value()].depth_in      = true; }
void Grammar::set_tab(NodeId id)     { nodes_[id.value()].indent_emit   = true; }
void Grammar::set_space(NodeId id)   { nodes_[id.value()].space_after   = true; }
void Grammar::set_newline(NodeId id) { nodes_[id.value()].newline_after = true; }
void Grammar::set_tail(NodeId id, std::string s) {
    nodes_[id.value()].tail = std::move(s);
}

// -------------------------------------------------------------------------
// Registries and accessors
// -------------------------------------------------------------------------

void Grammar::register_rule(std::string name, NodeId node) {
    named_rules_[std::move(name)] = node;
}

void Grammar::register_parser(std::unique_ptr<Parser> p) {
    std::string name = p->name();
    parsers_[name] = std::move(p);
    // If this parser was on the ignore list (host called add_ignore
    // before the parser was registered, or a parser group is replacing
    // an earlier registration), refresh the raw pointer so the
    // driver's ignore loop doesn't dereference the old (destroyed)
    // unique_ptr.
    for (std::size_t i = 0; i < ignore_names_.size(); ++i) {
        if (ignore_names_[i] == name) {
            ignore_[i] = parsers_[name].get();
        }
    }
    // Same possibility for rule-local override lists; rebuild lazily
    // on the next rule_ignore() call.
    rule_ignore_dirty_ = true;
}

void Grammar::register_parser_alias(std::string key, std::unique_ptr<Parser> p) {
    parsers_[std::move(key)] = std::move(p);
}

void Grammar::add_ignore(std::string parser_name) {
    auto it = parsers_.find(parser_name);
    assert(it != parsers_.end());
    ignore_.push_back(it->second.get());
    ignore_names_.push_back(std::move(parser_name));
}

void Grammar::add_rule_ignore(std::string rule_name,
                              std::vector<std::string> parser_names) {
    rule_ignore_names_[std::move(rule_name)] = std::move(parser_names);
    rule_ignore_dirty_ = true;
}

void Grammar::set_pending_subparse(NodeId target, std::string rule_name) {
    pending_subparse_.emplace_back(target, std::move(rule_name));
}

tl::expected<void, std::string> Grammar::resolve_subparse_refs() {
    for (const auto& [target, rule_name] : pending_subparse_) {
        auto it = named_rules_.find(rule_name);
        if (it == named_rules_.end()) {
            return tl::unexpected("subparse references undefined rule '" +
                                  rule_name + "'");
        }
        node(target).subparse_start = it->second;
    }
    pending_subparse_.clear();
    return {};
}

const std::vector<Parser*>* Grammar::rule_ignore(NodeId rule_node) const {
    if (rule_ignore_dirty_) {
        rule_ignore_resolved_.clear();
        for (const auto& [rule_name, parser_names] : rule_ignore_names_) {
            auto rit = named_rules_.find(rule_name);
            if (rit == named_rules_.end()) continue;
            std::vector<Parser*> ps;
            ps.reserve(parser_names.size());
            for (const auto& pn : parser_names) {
                auto pit = parsers_.find(pn);
                if (pit == parsers_.end()) continue;
                ps.push_back(pit->second.get());
            }
            rule_ignore_resolved_[rit->second.value()] = std::move(ps);
        }
        rule_ignore_dirty_ = false;
    }
    auto it = rule_ignore_resolved_.find(rule_node.value());
    return it == rule_ignore_resolved_.end() ? nullptr : &it->second;
}

void Grammar::set_top(NodeId node) {
    top_ = node;
}

const Node& Grammar::node(NodeId id) const noexcept {
    return nodes_[id.value()];
}

Node& Grammar::node(NodeId id) noexcept {
    return nodes_[id.value()];
}

NodeId Grammar::resolve_ref(NodeId id) const {
    // Fast path: lookup precomputed resolution table.
    if (resolved_refs_computed_) {
        return id.value() < resolved_refs_.size()
            ? resolved_refs_[id.value()]
            : id;
    }
    // Slow path (called pre-cache, e.g. during loader phases that
    // run before ensure_refs_resolved_).
    while (id.valid() && nodes_[id.value()].kind == NodeKind::Ref) {
        const auto& v = nodes_[id.value()].value;
        auto sv = std::dynamic_pointer_cast<StringValue>(v);
        assert(sv);
        auto it = named_rules_.find(sv->data());
        assert(it != named_rules_.end());
        id = it->second;
    }
    return id;
}

void Grammar::ensure_refs_resolved_() const {
    if (resolved_refs_computed_) return;
    const std::size_t N = nodes_.size();
    resolved_refs_.resize(N);
    for (std::size_t i = 0; i < N; ++i) {
        NodeId id{i};
        // Re-walk via the slow path (which is still active because
        // resolved_refs_computed_ is false). After this loop the
        // fast path takes over.
        while (id.valid() && nodes_[id.value()].kind == NodeKind::Ref) {
            const auto& v = nodes_[id.value()].value;
            auto sv = std::dynamic_pointer_cast<StringValue>(v);
            assert(sv);
            auto it = named_rules_.find(sv->data());
            assert(it != named_rules_.end());
            id = it->second;
        }
        resolved_refs_[i] = id;
    }
    resolved_refs_computed_ = true;
}

bool Grammar::has_rule(const std::string& name) const noexcept {
    return named_rules_.find(name) != named_rules_.end();
}

NodeId Grammar::rule_id(const std::string& name) const noexcept {
    auto it = named_rules_.find(name);
    if (it == named_rules_.end()) return NodeId{};
    return it->second;
}

Parser* Grammar::parser(const std::string& name) const {
    auto it = parsers_.find(name);
    return it == parsers_.end() ? nullptr : it->second.get();
}

std::vector<std::string> Grammar::parser_groups() const {
    // Every parser registered via apply_parser_group is registered TWICE
    // in parsers_: once under the bare name (e.g. "int") and once under
    // the dotted "group.bare" alias (e.g. "std.int"). The presence of the
    // dotted form is the only signal that the parser belongs to a group;
    // ad-hoc parsers registered without a group don't get dotted aliases.
    std::set<std::string> groups;
    for (const auto& [name, _ignored] : parsers_) {
        auto dot = name.find('.');
        if (dot == std::string::npos) continue;
        groups.insert(name.substr(0, dot));
    }
    return {groups.begin(), groups.end()};
}

// -------------------------------------------------------------------------
// Load driver — trampolined recursive descent
// -------------------------------------------------------------------------

namespace {

void run_ignore(StreamReader& sr, const std::vector<Parser*>& ignores) {
    // Loop until a full pass through the ignore list consumes nothing.
    // This handles arbitrary interleaving of whitespace and comments —
    // e.g. "  // line\n  /* block */\n  " requires multiple cycles
    // because each ignore parser only consumes one contiguous run.
    while (true) {
        const std::size_t before = sr.position().bytes;
        for (Parser* p : ignores) {
            (void)p->parse(sr);
        }
        if (sr.position().bytes == before) break;
    }
}

// Push a NodeId onto the stack, resolving Ref chains. If any Ref along
// the chain has is_optional=true (the `?<RULE>` site pattern), propagate
// that flag to the pushed Frame — without it the optional fails-as-
// empty handling in handle_failure would never fire for ref-site
// optionals.
void push_node(std::vector<Frame>& stack, const Grammar& g, NodeId id) {
    NodeId resolved = g.resolve_ref(id);
    stack.emplace_back(g, resolved);
    // Walk the Ref chain from the original use-site downward, looking
    // for is_optional / is_negative markers placed on a Ref by the
    // grammar source. Both flags propagate to the resolved Frame so
    // the driver can react to them in handle_failure / advance_after_child
    // even though the resolved Node itself is unmarked.
    NodeId cur = id;
    bool found_opt = false;
    bool found_neg = false;
    while (cur.valid() && cur.value() != resolved.value()
                       && g.node(cur).kind == NodeKind::Ref) {
        const Node& cn = g.node(cur);
        if (cn.is_optional && !found_opt) {
            stack.back().force_optional();
            found_opt = true;
        }
        if (cn.is_negative && !found_neg) {
            stack.back().force_negative();
            found_neg = true;
        }
        if (found_opt && found_neg) break;
        auto sv = std::dynamic_pointer_cast<StringValue>(cn.value);
        if (!sv) break;
        cur = g.rule_id(sv->data());
    }
}

// Update max_progress if the new error is further along.
void note_progress(ParseError& max_progress, const ParseError& err) {
    if (err.position.bytes > max_progress.position.bytes) {
        max_progress = err;
    }
}

// Recursively compact `{lhs, tail:[{op,rhs},...]}` always-wrap shapes
// into `{op, args[]}` with same-op runs collapsed and mixed-op
// boundaries nested.
//
// Algorithm:
//   * Atoms and non-dicts pass through unchanged (recursing into
//     arrays so list-shaped fields get compacted element-wise).
//   * Dicts have their fields recursively compacted first.
//   * If the resulting dict has `lhs` and a non-empty `tail`, fold
//     the tail left-to-right onto lhs:
//       - acc = lhs
//       - For each (op, rhs) in tail: if acc is `{op:OP, args:[...]}`
//         with same OP, extend args; otherwise wrap acc as
//         `{op:OP, args:[acc, rhs]}`.
//   * The original `lhs` / `tail` keys are dropped from the output;
//     any other keys on the wrapper carry through.
//   * Empty tail (or no tail at all): unwrap to `lhs`.
ValuePtr compact_opchain(const ValuePtr& v) {
    if (!v) return v;

    if (auto arr = as_array(v)) {
        auto out = std::make_shared<ArrayValue>();
        for (const auto& elt : arr->data()) {
            out->data().push_back(compact_opchain(elt));
        }
        return out;
    }

    auto d = as_dict(v);
    if (!d) return v;

    // Recurse on field values first.
    auto recursed = std::make_shared<DictValue>();
    for (const auto& [k, val] : d->data()) {
        recursed->data().emplace(k, compact_opchain(val));
    }

    auto lhs_it = recursed->data().find("lhs");
    if (lhs_it == recursed->data().end()) {
        // Not an always-wrap shape; return with compacted fields.
        return recursed;
    }
    ValuePtr lhs = lhs_it->second;

    auto tail_it = recursed->data().find("tail");
    auto tail_arr = (tail_it != recursed->data().end())
        ? as_array(tail_it->second) : nullptr;
    bool has_tail = tail_arr && !tail_arr->data().empty();

    // Any non-`lhs`/`tail` field signals a non-opchain shape (e.g.
    // ASSIGNMENT_PAIR's `{lhs, rhs}`) — leave it alone. Unwrapping
    // those would silently merge `lhs`'s inner fields with the
    // sibling and lose structure.
    bool has_other = false;
    for (const auto& [k, _] : recursed->data()) {
        if (k != "lhs" && k != "tail") { has_other = true; break; }
    }
    if (has_other) {
        return recursed;
    }

    if (!has_tail) {
        // Empty / absent `tail` → no operators ran, just pass the lhs
        // through unchanged.
        return lhs;
    }

    // Non-binary tail entry (e.g. `inside {..}` carries `body`, not
    // `rhs`) — can't fold to `{op,args}`. Leave the whole chain raw so
    // the always-wrap save path handles it untouched; cascade-aware
    // expand never sees a half-folded inside chain.
    for (const auto& te : tail_arr->data()) {
        auto td = as_dict(te);
        if (!td || td->data().find("rhs") == td->data().end()) {
            return recursed;
        }
    }

    // Fold the tail left-to-right.
    ValuePtr acc = lhs;
    for (const auto& te : tail_arr->data()) {
        auto td = as_dict(te);
        if (!td) continue;
        auto op_it  = td->data().find("op");
        auto rhs_it = td->data().find("rhs");
        if (op_it == td->data().end() || rhs_it == td->data().end()) continue;
        auto op_sv = as_string(op_it->second);
        if (!op_sv) continue;
        const std::string& op = op_sv->data();
        ValuePtr rhs = rhs_it->second;

        // Extend acc's args if it already has the same op.
        bool extended = false;
        if (auto ad = as_dict(acc)) {
            auto ao_it = ad->data().find("op");
            auto aa_it = ad->data().find("args");
            if (ao_it != ad->data().end() && aa_it != ad->data().end()) {
                auto ao_sv = as_string(ao_it->second);
                auto aa_arr = as_array(aa_it->second);
                if (ao_sv && aa_arr && ao_sv->data() == op
                    // Same-op collapse only if no nested mixed-op
                    // boundary lies inside — i.e. args contains only
                    // values produced by THIS fold (not a user-given
                    // {op,args} dict from the input). The fold itself
                    // produces nested same-op dicts only via this
                    // branch, so reaching here implies acc was either
                    // the original lhs (atom) or a previously
                    // extended same-op chain. Either case is safe.
                ) {
                    auto new_args = std::make_shared<ArrayValue>();
                    for (const auto& a : aa_arr->data()) {
                        new_args->data().push_back(a);
                    }
                    new_args->data().push_back(rhs);

                    auto new_acc = std::make_shared<DictValue>();
                    new_acc->data().emplace("op", ao_it->second);
                    new_acc->data().emplace("args", new_args);
                    acc = new_acc;
                    extended = true;
                }
            }
        }
        if (extended) continue;

        // Different op (or first iteration): wrap acc as
        // `{op:OP, args:[acc, rhs]}`.
        auto new_args = std::make_shared<ArrayValue>();
        new_args->data().push_back(acc);
        new_args->data().push_back(rhs);

        auto new_acc = std::make_shared<DictValue>();
        new_acc->data().emplace("op", make_string(op));
        new_acc->data().emplace("args", new_args);
        acc = new_acc;
    }

    if (!has_other) return acc;

    // Carry through extra wrapper fields.
    auto out = std::make_shared<DictValue>();
    if (auto ad = as_dict(acc)) {
        for (const auto& [k, v2] : ad->data()) out->data().emplace(k, v2);
    }
    for (const auto& [k, v2] : recursed->data()) {
        if (k == "lhs" || k == "tail") continue;
        out->data().emplace(k, v2);
    }
    return out;
}

// Unified byte-scan routine. Both Raw (`*`) and Scope (`scope { … }`)
// dispatch through this — they materialise ScanConfig values that
// describe the same operation (scan from cursor with optional start
// delimiter, stop literal, atomic-span INNERs, string-or-array output).
//
// INNERs are atomic: when an INNER succeeds, the matched bytes are
// folded into the captured payload and stop-matching skips over them.
// This is what makes embedded `std.string`, `std.line_comment`, etc.
// transparent to the scope — a `")"` inside a string doesn't end the
// scope because the string parser swallowed it.
//
// Mark discipline: the caller MUST hold a live mark at scope entry;
// the routine does its own internal marks for start, stop, and INNER
// trials and balances them. On failure return, the stream is left
// wherever the failure happened — the caller's outer mark provides
// the rewind.
inline bool scope_is_word_char(char c) noexcept {
    auto u = static_cast<unsigned char>(c);
    return (u >= '0' && u <= '9') ||
           (u >= 'A' && u <= 'Z') ||
           (u >= 'a' && u <= 'z') || u == '_';
}

// Build a ScanConfig from a Scope node. Scope is structurally Raw +
// INNERs + optional array container: no opening delimiter (siblings
// in the surrounding sequence carry start/stop literals), stops
// resolved at load time by `resolve_raw_stops` and stashed on
// `n.stops`, sibling consumes the stop (consume_stop=false).
ScanConfig scan_config_from_scope(const Node& n) {
    ScanConfig cfg;
    cfg.stops          = n.stops;
    cfg.stop_strict    = n.stops_strict;
    cfg.inners         = n.children;
    cfg.container      = n.container;
    cfg.subparse_start = n.subparse_start;
    cfg.consume_stop   = false;
    return cfg;
}

// Build a ScanConfig from a Raw node. Raw's pre-resolved sibling-stop
// literals are stashed on `n.stops` by `resolve_raw_stops` at load
// time. Raw has no INNERs, is always string-output, and leaves the
// matched stop literal unconsumed for the next sibling Key to match.
ScanConfig scan_config_from_raw(const Node& n) {
    ScanConfig cfg;
    cfg.stops          = n.stops;
    cfg.stop_strict    = n.stops_strict;
    cfg.subparse_start = n.subparse_start;
    cfg.consume_stop   = false;
    return cfg;
}

tl::expected<ValuePtr, ParseError> walk_scan(
    const Grammar& g, StreamReader& sr, ValuePool& pool,
    const ScanConfig& cfg) {
    const Position entry = sr.position();
    const bool array_mode = (cfg.container == Container::Array);

    const std::string& open_str  = cfg.start;
    if (cfg.stops.empty()) {
        return tl::unexpected(ParseError{
            entry, "scan: no stop literals — the load-time stop "
                   "resolver couldn't find any Key sibling that bounds "
                   "this scope/raw"});
    }
    // Fast path for single-stop (the overwhelmingly common case —
    // every direct-child scope/raw, all single-stop Raw `*` uses).
    // The byte-scan hot loop hits per cursor byte; avoiding the
    // 256-byte stack array init on every walk_scan call recovers
    // ~95%+ of the throughput vs the multi-stop fallback.
    const bool single_stop = (cfg.stops.size() == 1);
    const std::string& single_close = single_stop ? cfg.stops[0] : std::string{};
    const char single_first = single_stop && !single_close.empty()
                              ? single_close[0] : '\0';
    std::array<bool, 256> stop_first_byte{};
    if (!single_stop) {
        for (const auto& s : cfg.stops) {
            if (s.empty()) continue;
            stop_first_byte[static_cast<unsigned char>(s[0])] = true;
        }
    }

    // Match start (if present). No ignore-set skipping inside the
    // scan; embedded whitespace is part of the body.
    if (!open_str.empty()) {
        sr.mark();
        for (char c : open_str) {
            auto g_ = sr.get();
            if (!g_ || *g_ != c) {
                sr.reject();
                return tl::unexpected(ParseError{
                    entry, "scan: expected start literal '" + open_str + "'"});
            }
        }
        if (cfg.start_strict && !open_str.empty()
            && scope_is_word_char(open_str.back())) {
            auto next = sr.peek();
            if (next && scope_is_word_char(*next)) {
                sr.reject();
                return tl::unexpected(ParseError{
                    entry, "scan: strict start '" + open_str +
                            "' requires word boundary"});
            }
        }
        sr.accept();
    }

    // Accumulators. String mode uses `body` (single concatenated payload).
    // Array mode uses `segments` (ordered list of mixed StringValue text
    // runs + INNER-typed values), with `text_run` buffering raw bytes
    // until the next INNER match or stop flushes it.
    std::string body;
    auto       segments = array_mode ? std::make_shared<ArrayValue>() : nullptr;
    std::string text_run;

    auto flush_text_run = [&]() {
        if (text_run.empty()) return;
        segments->data().push_back(make_string(std::move(text_run)));
        text_run.clear();
    };

    while (true) {
        auto pk = sr.peek();
        if (!pk) {
            std::string msg = "scan: unterminated, no stop (";
            for (std::size_t i = 0; i < cfg.stops.size(); ++i) {
                if (i) msg += " | ";
                msg += "'" + cfg.stops[i] + "'";
            }
            msg += ") before EOF";
            return tl::unexpected(ParseError{entry, std::move(msg)});
        }

        // Stop check — single-stop fast path (compiles to a byte
        // compare + memcmp; same shape as the pre-multi-stop engine);
        // multi-stop path consults the per-stop first-byte set and
        // loops over candidate stops only when the first byte matches.
        bool stop_hit = false;
        if (single_stop) {
            if (*pk == single_first) {
                sr.mark();
                bool ok = true;
                for (char e : single_close) {
                    auto gc = sr.get();
                    if (!gc || *gc != e) { ok = false; break; }
                }
                if (ok && cfg.stop_strict
                    && scope_is_word_char(single_close.back())) {
                    auto next = sr.peek();
                    if (next && scope_is_word_char(*next)) ok = false;
                }
                if (ok) { stop_hit = true; }
                else    { sr.reject(); }
            }
        } else if (stop_first_byte[static_cast<unsigned char>(*pk)]) {
            for (const auto& close_str : cfg.stops) {
                if (close_str.empty()) continue;
                if (*pk != close_str[0]) continue;
                sr.mark();
                bool ok = true;
                for (char e : close_str) {
                    auto gc = sr.get();
                    if (!gc || *gc != e) { ok = false; break; }
                }
                if (ok && cfg.stop_strict
                    && scope_is_word_char(close_str.back())) {
                    auto next = sr.peek();
                    if (next && scope_is_word_char(*next)) ok = false;
                }
                if (ok) { stop_hit = true; break; }
                sr.reject();
            }
        }
        if (stop_hit) {
            // consume_stop=true (Scope) accepts the mark so the stop
            // literal stays consumed; consume_stop=false (Raw) rejects
            // to leave the stop for the next sibling to match.
            if (cfg.consume_stop) sr.accept();
            else                  sr.reject();
            if (array_mode) {
                flush_text_run();
                return ValuePtr(segments);
            }
            return ValuePtr(make_string(std::move(body)));
        }

        // Try each INNER in order.
        bool inner_ok = false;
        for (NodeId inner_id : cfg.inners) {
            const Node& inner = g.node(g.resolve_ref(inner_id));

            if (inner.kind == NodeKind::Parse) {
                auto sv = std::dynamic_pointer_cast<StringValue>(inner.value);
                if (!sv) continue;
                Parser* p = g.parser(sv->data());
                if (!p) continue;
                p->reset();
                const Position before = sr.position();
                auto wr = p->walk(sr);
                if (wr) {
                    if (array_mode) {
                        flush_text_run();
                        // INNER's typed value lands as one segment.
                        segments->data().push_back(p->value());
                    } else {
                        // Capture the FULL matched bytes (delimiters and
                        // all) by reading the stream's retained window
                        // — text_value() reports the parser's accumulator,
                        // which strips delimiters (e.g. `"..."` outer
                        // quotes) the body needs to round-trip verbatim.
                        body.append(sr.bytes_from(before));
                    }
                    inner_ok = true;
                    break;
                }
                // walk() rewound on failure (its own mark/reject);
                // try next INNER.
            } else {
                // Any other rule shape (Sequence / Choice / Repeat / Ref
                // chain that resolves to one) — re-enter the parse driver
                // via parse_from. The resolved Node id is the rule's
                // entry; on success we get its typed value back.
                //
                // Mark/reject isolates the trial: parse_from may consume
                // bytes before failing, so we wrap with our own mark.
                // Zero-byte matches are rejected — silently accepting
                // them would loop forever on a nullable rule.
                sr.mark();
                const Position before = sr.position();
                auto sub_r = g.parse_from(sr, pool, g.resolve_ref(inner_id),
                                          /*require_full_consume=*/false,
                                          cfg.caller_ignore);
                if (sub_r && sr.position().bytes > before.bytes) {
                    sr.accept();
                    if (array_mode) {
                        flush_text_run();
                        segments->data().push_back(*sub_r);
                    } else {
                        body.append(sr.bytes_from(before));
                    }
                    inner_ok = true;
                    break;
                }
                sr.reject();
            }
        }
        if (inner_ok) continue;

        // No INNER matched. Consume one raw byte.
        auto cb = sr.get();
        if (array_mode) text_run.push_back(*cb);
        else            body.push_back(*cb);
    }
}

} // namespace

tl::expected<ValuePtr, ParseError> Grammar::parse(Stream& stream) const {
    ValuePool pool;
    return parse(stream, pool);
}

tl::expected<ValuePtr, ParseError> Grammar::parse(Stream& stream, ValuePool& pool) const {
    return parse_from(stream.reader(), pool, top_);
}

tl::expected<ValuePtr, ParseError> Grammar::parse(std::string text) const {
    auto stream = Stream::from_string(std::move(text));
    return parse(stream);
}

tl::expected<ValuePtr, ParseError> Grammar::parse(std::string text, ValuePool& pool) const {
    auto stream = Stream::from_string(std::move(text));
    return parse(stream, pool);
}

tl::expected<ValuePtr, ParseError> Grammar::parse_from(
        Stream& stream, const std::string& start_name) const {
    ValuePool pool;
    return parse_from(stream, pool, start_name);
}

tl::expected<ValuePtr, ParseError> Grammar::parse_from(
        Stream& stream, ValuePool& pool, const std::string& start_name) const {
    auto it = named_rules_.find(start_name);
    if (it == named_rules_.end()) {
        return tl::unexpected(ParseError{
            stream.reader().position(),
            "parse_from: no rule named '" + start_name + "'"});
    }
    return parse_from(stream.reader(), pool, it->second);
}

tl::expected<ValuePtr, ParseError> Grammar::parse_from(
        Stream& stream, ValuePool& pool, NodeId start,
        bool require_full_consume,
        const std::vector<Parser*>* initial_ignore) const {
    return parse_from(stream.reader(), pool, start,
                       require_full_consume, initial_ignore);
}

tl::expected<ValuePtr, ParseError> Grammar::parse_from(
        StreamReader& sr, ValuePool& pool, NodeId start,
        bool require_full_consume,
        const std::vector<Parser*>* initial_ignore) const {
    std::vector<Frame> stack;
    ParseError max_progress{sr.position(), "no parse attempted"};
    ValuePtr result_value;
    bool parse_finished = false;   // true once the top frame pops successfully

    // Trace mode: enabled via `RAWAST_TRACE` env var. When set, the
    // parse driver emits per-frame events to stderr with byte/line
    // positions and rule names — useful for grammar debugging
    // ("which rule were you in when you failed?"). Zero cost when
    // off: one env-var read at parse start, one bool check at each
    // instrumentation site. See docs/debugging.md.
    const bool trace_enabled = std::getenv("RAWAST_TRACE") != nullptr;

    // Profiling mode: enabled via Grammar::profile_enable(). When on,
    // the loop tracks per-Node entry counts, fail counts, wall-clock
    // time spent on the parse stack, and max depth. Results land on
    // `last_profile_report_` at the end of the call; the public API
    // exposes them via Grammar::last_profile_report(). Off by
    // default — one bool check at each instrumentation site.
    const bool profile_enabled = profile_enabled_;
    std::vector<ProfileEntry>                        profile_per_node;
    std::vector<std::chrono::steady_clock::time_point> profile_frame_starts;
    std::vector<NodeId>                               profile_frame_ids;
    std::uint64_t                                     profile_total_frames = 0;
    std::uint64_t                                     profile_max_depth    = 0;
    const auto                                        profile_start_clock =
        std::chrono::steady_clock::now();
    if (profile_enabled) {
        profile_per_node.assign(nodes_.size(), ProfileEntry{});
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            profile_per_node[i].node_id = NodeId{i};
        }
    }

    // Sync the profile state against the live stack. Called at the
    // top of every loop iteration: detects push events (stack grew
    // since last sync — record start time, bump entry_count, update
    // max_depth) and pop events (stack shrunk — accumulate elapsed
    // wall-clock time for each popped frame back into its NodeId's
    // entry). Idempotent if the stack hasn't changed.
    auto profile_sync = [&]() {
        if (!profile_enabled) return;
        while (profile_frame_starts.size() < stack.size()) {
            std::size_t idx = profile_frame_starts.size();
            profile_frame_starts.push_back(std::chrono::steady_clock::now());
            NodeId nid = stack[idx].node_id();
            profile_frame_ids.push_back(nid);
            ++profile_total_frames;
            std::size_t v = nid.value();
            if (v < profile_per_node.size()) {
                ++profile_per_node[v].entry_count;
                if (stack.size() > profile_per_node[v].max_depth) {
                    profile_per_node[v].max_depth = stack.size();
                }
            }
        }
        if (stack.size() > profile_max_depth) profile_max_depth = stack.size();
        while (profile_frame_starts.size() > stack.size()) {
            auto end = std::chrono::steady_clock::now();
            auto elapsed_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - profile_frame_starts.back()).count();
            NodeId nid = profile_frame_ids.back();
            std::size_t v = nid.value();
            if (v < profile_per_node.size()) {
                profile_per_node[v].total_ns += elapsed_ns;
            }
            profile_frame_starts.pop_back();
            profile_frame_ids.pop_back();
        }
    };

    // Reverse map NodeId → rule name, built once at parse start when
    // tracing is enabled. Used by node_label() to produce
    // human-readable trace messages. Empty when tracing is off (no
    // allocation cost).
    std::map<std::uint32_t, std::string> node_to_rule;
    if (trace_enabled) {
        for (const auto& [name, id] : named_rules_) {
            node_to_rule[id.value()] = name;
        }
    }

    auto node_label = [&](NodeId id) -> std::string {
        auto it = node_to_rule.find(id.value());
        if (it != node_to_rule.end()) return it->second;
        // Anonymous (inline) node — show kind + node id for context.
        const Node& n = nodes_[id.value()];
        const char* kind = "?";
        switch (n.kind) {
            case NodeKind::Sequence: kind = "seq"; break;
            case NodeKind::Choice:   kind = "choice"; break;
            case NodeKind::Repeat:   kind = "repeat"; break;
            case NodeKind::Key:      kind = "key"; break;
            case NodeKind::Parse:    kind = "parse"; break;
            case NodeKind::Ref:      kind = "ref"; break;
            case NodeKind::Value:    kind = "value"; break;
            case NodeKind::Raw:      kind = "raw"; break;
            case NodeKind::Scope:    kind = "scope"; break;
        }
        return std::string{kind} + "#" + std::to_string(id.value());
    };

    auto trace_pos = [&]() -> std::string {
        auto p = sr.position();
        return "L" + std::to_string(p.line)
             + ":C" + std::to_string(p.column);
    };

    auto trace = [&](const std::string& msg) {
        if (!trace_enabled) return;
        // Indent by stack depth so the trace is visually nested.
        std::cerr << "[" << trace_pos() << "] ";
        for (std::size_t i = 0; i < stack.size(); ++i) std::cerr << "  ";
        std::cerr << msg << "\n";
    };

    // Sequence-success cache --------------------------------------------
    //
    // Append-only vector of `(node, start_offset, end_offset, emitted)`
    // entries captured at the moment a frame successfully completes.
    // The cache is consulted at every push site (via push_or_skip_optional)
    // — when the top entry matches the resolved node at the current
    // offset, the frame is skipped entirely and its emissions are
    // replayed to the parent. This kills the doubling at every Choice
    // {CHAIN, NEXT} retry in deep cascade ladders (e.g. SV's 12-level
    // expression chain). Memory is bounded because each successful
    // frame's pop retires the interior entries it accumulated.
    struct CacheEntry {
        NodeId      node;
        std::size_t start_offset;
        std::size_t end_offset;
        std::vector<Frame::EmittedValue> emitted;
    };
    std::vector<CacheEntry> cache;

    // Rule-local ignore overrides ---------------------------------------
    //
    // An IgnoreScope captures "stack depth at which this override was
    // pushed". After every pop, the loop discards scopes whose depth
    // exceeds the current stack size — i.e. whose owning frame is gone.
    // The active ignore list is the top of the stack, or the
    // grammar-level default if the stack is empty.
    struct IgnoreScope {
        std::size_t entry_depth;
        const std::vector<Parser*>* parsers;
    };
    std::vector<IgnoreScope> ignore_stack;
    // Seed the caller's active ignore policy when invoked from a
    // walk_scan INNER subparse. entry_depth=0 keeps the seed at the
    // bottom of the stack for the lifetime of this parse — any rule
    // that pushes its own override sits on top until popped, then
    // the seed re-surfaces. With the seed, all run_ignore sites
    // (predictive, between-items, end-of-parse) see the caller's
    // policy uniformly — no asymmetry between optional-boundary
    // checks and inter-item whitespace skipping.
    if (initial_ignore && !initial_ignore->empty()) {
        ignore_stack.push_back({/*entry_depth=*/0, initial_ignore});
    }
    auto current_ignore = [&]() -> const std::vector<Parser*>& {
        return ignore_stack.empty() ? ignore() : *ignore_stack.back().parsers;
    };
    // First-content guard. When this parse_from is a walk_scan INNER
    // subparse (initial_ignore set), the leading run_ignore calls
    // at Key / Parse / Raw cases are suppressed until the rule has
    // consumed its first byte of content. This keeps leading
    // whitespace in the surrounding scope's body capture instead of
    // being eaten by the INNER's leading run_ignore. After any
    // terminal content match, the guard clears and subsequent
    // run_ignore calls fire normally — INNERs with required
    // inter-item whitespace tolerance (e.g. `"begin" ident "end"`)
    // work as if dispatched via a normal Ref.
    const bool is_subparse = (initial_ignore != nullptr);
    bool has_consumed_content = !is_subparse;
    // Wrapper for the per-cursor run_ignore at content sites
    // (predictive checks, Key / Parse / Raw / scope entry). Defers
    // to current_ignore() once content has been consumed; otherwise
    // (subparse, before first content) becomes a no-op so leading
    // whitespace stays in the surrounding scope's body capture.
    auto run_ignore_guarded = [&]() {
        if (has_consumed_content) run_ignore(sr, current_ignore());
    };
    auto trim_ignore_stack = [&]() {
        while (!ignore_stack.empty()
               && ignore_stack.back().entry_depth > stack.size()) {
            ignore_stack.pop_back();
        }
    };

    // Wrap a push_node so optional frames pick up a stream mark on entry.
    // Without this, an optional sub-rule that consumes input then fails
    // mid-parse would leave the stream advanced — the "treat as empty"
    // path in handle_failure would never rewind. Parallel to Choice's
    // mark/reject around alternative attempts.
    // Lazily compute per-Node first-byte sets the first time we need
    // them. Cached on the Grammar across parses.
    auto ensure_first_bytes = [this]() {
        if (!first_bytes_computed_) compute_first_bytes();
    };

    // True if the about-to-be-pushed node is an optional whose
    // first-byte set is known and doesn't include the next input
    // byte. Lets the parse driver skip the push + key-parse + rewind
    // for `?<MASK_MOD>` / `?<ITERATE_PREFIX>` / `?<STEP_PATTERN>` and
    // every other optional clause that doesn't fire on the current
    // input. Profiling showed those three alone burn ~14% of the
    // wall-clock parse time on a real Sky130 / asap7 / gf130bcd
    // corpus, all at 100% fail rate.
    //
    // Conservative: returns false unless the optionality AND the
    // first-byte miss are both confirmed. Anything ambiguous falls
    // back to the normal push-and-try path, preserving correctness.
    // Choice-alt peek-and-skip. Distinct from should_skip_optional:
    // applies to ANY Choice alternative regardless of optional-chain
    // status — at a Choice dispatch site, an alt whose first-byte
    // set is known and doesn't include the next input byte cannot
    // possibly match, so we can step past it without pushing. The
    // big payoff is on 46-alt closed-keyword choices like
    // LAYER_KEYWORD where ~97% of attempts on real LEFs rewind on
    // the first character of the key.
    auto choice_alt_cant_match = [this, &sr,
                                   &ensure_first_bytes,
                                   &run_ignore_guarded](NodeId alt) -> bool {
        ensure_first_bytes();
        NodeId resolved = resolve_ref(alt);
        if (resolved.value() >= first_bytes_known_.size()) return false;
        if (!first_bytes_known_[resolved.value()])         return false;
        run_ignore_guarded();
        auto c = sr.peek();
        if (!c) return false;
        return !first_bytes_[resolved.value()].test(
            static_cast<unsigned char>(*c));
    };

    auto should_skip_optional = [&](NodeId id) -> bool {
        // Fast path: precomputed flag tells us in O(1) whether this
        // node is in an `?<...>` use-site optional chain. Non-
        // optional pushes (the vast majority — Sequence children,
        // Choice alts, Repeat items) return immediately without
        // touching the input stream.
        ensure_first_bytes();
        if (id.value() >= is_optional_chain_.size()) return false;
        if (!is_optional_chain_[id.value()])         return false;
        NodeId resolved = resolve_ref(id);
        if (resolved.value() >= first_bytes_known_.size()) return false;
        if (!first_bytes_known_[resolved.value()])         return false;
        // Peek the next byte after ignore-skip. Whitespace skipping
        // is idempotent; if we later push the frame, the inner
        // terminal re-runs ignore harmlessly. current_ignore()
        // reflects the caller's seeded policy for subparses; the
        // first-content guard suppresses the run_ignore until a
        // terminal has matched, so optional-at-start of an INNER
        // doesn't leak leading whitespace into the INNER's span.
        run_ignore_guarded();
        auto c = sr.peek();
        if (!c) return false;   // EOF — let the engine handle
        bool skip = !strict_first_bytes_[resolved.value()].test(
            static_cast<unsigned char>(*c));
        return skip;
    };

    auto push_with_optional_mark = [&](NodeId id) {
        std::size_t entry_offset = sr.position().bytes;
        push_node(stack, *this, id);
        if (!stack.empty()) {
            stack.back().set_cache_size_at_push(cache.size());
            stack.back().set_start_offset(entry_offset);
        }
        if (trace_enabled && !stack.empty()) {
            const auto& f = stack.back();
            std::string opt = f.is_optional() ? " ?" : "";
            trace("PUSH " + node_label(f.node_id()) + opt);
        }
        if (!stack.empty() && stack.back().is_optional()
            && !stack.back().has_mark()) {
            sr.mark();
            stack.back().set_has_mark(true);
        }
        // Negative-lookahead entry: take a stream mark so the cursor
        // can be rolled back regardless of whether the inner matches.
        // Uses a SEPARATE mark slot (has_neg_mark_) so a `!<CHOICE>`
        // doesn't conflict with the Choice's own backtrack mark.
        if (!stack.empty() && stack.back().is_negative()
            && !stack.back().has_neg_mark()) {
            sr.mark();
            stack.back().set_has_neg_mark(true);
        }
        // Rule-local ignore: if the just-pushed frame's node has an
        // override registered, push it. Before pushing the override,
        // run the OUTER (caller's) `run_ignore` so whitespace at a
        // rule boundary gets eaten by the policy active when this
        // frame was about to be entered. Without this step, a rule
        // with `ignore:` (empty, suspending the inherited policy)
        // entered via a Ref would never see its caller's ignore
        // applied at the boundary.
        if (!stack.empty()) {
            if (const auto* ovr = rule_ignore(stack.back().node_id())) {
                run_ignore_guarded();
                ignore_stack.push_back({stack.size(), ovr});
            }
        }
    };

    // Forward-declared so advance_after_child can invoke the failure
    // path for a negative-lookahead success. Assigned to the actual
    // lambda below.
    std::function<void(const ParseError&)> handle_failure_fn;
    auto handle_failure = [&](const ParseError& err) { handle_failure_fn(err); };

    // Record a successful frame's result in the cache. Called between
    // popped.finish() and popped.pass_values_to(). Retires every cache
    // entry appended while this frame was live (they're all inside the
    // frame's byte range and unreachable now that the outer match
    // committed) — keeping the cache bounded — then appends this
    // frame's own entry so future re-entries at the same offset hit.
    auto record_cache_entry = [&](const Frame& popped) {
        cache.resize(popped.cache_size_at_push());
        std::size_t end_offset = sr.position().bytes;
        // Skip caching zero-consumption frames (Value, empty optionals,
        // skipped predictives). They have no work worth memoizing, and
        // letting them sit at the cache top blocks real entries beneath
        // from being found via the top-only lookup. (No information is
        // lost — a re-parse of a zero-consumption frame is free anyway.)
        if (end_offset == popped.start_offset()) return;
        CacheEntry entry;
        entry.node = popped.node_id();
        entry.start_offset = popped.start_offset();
        entry.end_offset = end_offset;
        entry.emitted = popped.emitted();
        cache.push_back(std::move(entry));
    };

    // Advance after a child has just completed successfully. Pops frames
    // until we either find one with more work to do, or reach the top.
    auto advance_after_child = [&]() {
        while (!stack.empty()) {
            Frame& top = stack.back();
            bool more = false;
            switch (top.kind()) {
            case NodeKind::Choice:
                // Choice's child succeeded -> choice itself is done. If
                // this Choice was backtracking and had marked the stream
                // for the just-succeeded alternative, accept that mark.
                if (top.has_mark()) {
                    sr.accept();
                    top.set_has_mark(false);
                }
                more = false;
                break;
            case NodeKind::Repeat:
                // Repeat case: accept the just-completed iteration's
                // mark (if any) before stepping. Each successful
                // iteration commits its consumption; a fresh mark is
                // pushed when the next iteration starts (in the
                // Repeat case at the main switch). This is what makes
                // a failing iteration roll back to the position
                // BEFORE the iteration started — without it, a Choice
                // inside the body that committed (accepted its own
                // mark) would leave the cursor advanced even if a
                // later required item failed.
                if (top.has_mark()) {
                    sr.accept();
                    top.set_has_mark(false);
                }
                more = top.step_next();
                break;
            case NodeKind::Sequence:
            case NodeKind::Key:
            case NodeKind::Parse:
            case NodeKind::Raw:
                // Key, Parse, and Raw iterate their (typically Value-
                // kind) children after the terminal succeeds; same
                // iteration model as Sequence.
                more = top.step_next();
                break;
            default:
                break;
            }
            if (more) return;

            // Pop this frame, finish, pass values up.
            Frame popped = std::move(stack.back());
            stack.pop_back();
            // Negative-lookahead success path: the inner just succeeded,
            // which inverts to FAILURE for the lookahead. Reject the
            // entry mark to rewind the cursor (consume nothing) and
            // propagate the failure upward through handle_failure so an
            // enclosing Choice / optional / Repeat can react. Do this
            // BEFORE the normal finish/pass-values path — there's no
            // value to emit because the lookahead's semantic outcome is
            // "no", not "yes with empty payload."
            if (popped.is_negative()) {
                if (popped.has_neg_mark()) {
                    sr.reject();
                    popped.set_has_neg_mark(false);
                }
                if (trace_enabled) {
                    trace("  negative-lookahead matched (failing)");
                }
                handle_failure(ParseError{
                    sr.position(),
                    "negative lookahead matched"});
                return;
            }
            // Optional frame completed successfully — accept the entry
            // mark so the stream advances permanently.
            if (popped.is_optional() && popped.has_mark()) {
                sr.accept();
                popped.set_has_mark(false);
            }
            popped.finish(pool);
            record_cache_entry(popped);
            if (stack.empty()) {
                result_value = popped.result();
                parse_finished = true;
                return;
            }
            popped.pass_values_to(stack.back());
            // Loop to advance the new top frame.
        }
    };

    // Push `child_id` or, if it's an optional whose first-byte set
    // doesn't include the next input byte, advance the parent's
    // iteration instead. Same observable effect as pushing a
    // doomed-to-fail frame and recovering through the optional
    // fallback path — just skips the frame churn. Use this in
    // place of `push_with_optional_mark` at the dispatch sites
    // where the next child can be optional.
    // Cache-hit short-circuit. If the cache top matches the resolved
    // node at the current stream offset, replay its emissions into the
    // parent and advance the cursor — skipping the frame push and all
    // the work that would happen inside it. Returns true on hit.
    auto try_cache_hit = [&](NodeId id) -> bool {
        if (cache.empty() || stack.empty()) return false;
        NodeId resolved = resolve_ref(id);
        std::size_t offset = sr.position().bytes;
        const CacheEntry& top = cache.back();
        if (top.node.value() != resolved.value()) return false;
        if (top.start_offset != offset) return false;
        // Replay emissions into the parent frame.
        Frame& parent = stack.back();
        for (const auto& ev : top.emitted) {
            parent.add_value(ev.value, ev.is_name);
        }
        // Advance the cursor to where the cached frame ended.
        while (sr.position().bytes < top.end_offset && !sr.eof()) sr.get();
        advance_after_child();
        return true;
    };

    auto push_or_skip_optional = [&](NodeId child_id) {
        if (should_skip_optional(child_id)) {
            if (stack.empty()) return;
            bool more = stack.back().step_next();
            if (!more) advance_after_child();
        } else if (try_cache_hit(child_id)) {
            // Cache hit handled the frame in full.
        } else {
            push_with_optional_mark(child_id);
        }
    };

    // Handle a parse failure: walk back to the nearest Choice with more
    // alternatives, or to a Repeat (which terminates gracefully on
    // failure), or to an optional level (which treats failure as empty).
    //
    // Order matters: Choice must be checked BEFORE the is_optional
    // fast-path, otherwise an optional Choice (e.g. `?<X>` where X is
    // itself a choice) would treat the first alt's failure as the whole
    // optional failing — never giving the Choice a chance to try
    // remaining alts.
    handle_failure_fn = [&](const ParseError& err) {
        if (trace_enabled) {
            trace("FAIL: " + err.message);
        }
        note_progress(max_progress, err);
        while (!stack.empty()) {
            if (profile_enabled) {
                std::size_t v = stack.back().node_id().value();
                if (v < profile_per_node.size()) {
                    ++profile_per_node[v].fail_count;
                }
            }
            Frame popped = std::move(stack.back());
            stack.pop_back();
            if (trace_enabled) {
                trace("  unwind " + node_label(popped.node_id()));
            }

            if (popped.kind() == NodeKind::Choice) {
                // The just-failed alternative was wrapped in a mark by
                // the entry-side code below (only if backtrack was on).
                // Reject it now so the stream rewinds to the position
                // before this alternative was tried.
                if (popped.has_mark()) {
                    sr.reject();
                    popped.set_has_mark(false);
                }
                if (popped.step_next()) {
                    if (trace_enabled) {
                        trace("  retry " + node_label(popped.node_id())
                              + " next-alt");
                    }
                    stack.push_back(std::move(popped));
                    return;
                }
                // Else: choice exhausted -- propagate the failure further.
                // If the choice was optional, fall through to the optional
                // handler below.
            }

            if (popped.is_negative()) {
                // Negative-lookahead failure path: the inner failed,
                // which inverts to SUCCESS for the lookahead. Reject
                // the neg-entry mark to rewind the cursor (the inner
                // may have consumed bytes before failing) and treat
                // as success-with-empty. The check comes BEFORE the
                // is_optional branch so a frame that is both `?` and
                // `!` (legal but unusual) gets the negative inversion
                // first, which is the user-facing surface meaning of
                // `!` — the result of the operator must be empty, not
                // a partial inner match.
                if (popped.has_neg_mark()) {
                    sr.reject();
                    popped.set_has_neg_mark(false);
                }
                if (trace_enabled) {
                    trace("  negative-lookahead inner failed (succeeding empty)");
                }
                if (!stack.empty()) {
                    advance_after_child();
                }
                return;
            }

            if (popped.is_optional()) {
                // Reject the entry mark to rewind the stream to where
                // the optional started — without this, any consumed
                // input before the inner failure is left dangling.
                if (popped.has_mark()) {
                    sr.reject();
                    popped.set_has_mark(false);
                }
                // Treat as success-with-empty.
                if (!stack.empty()) {
                    advance_after_child();
                }
                return;
            }

            if (popped.kind() == NodeKind::Repeat) {
                // Reject the failed iteration's mark FIRST — this
                // rolls the cursor back to the position before the
                // iteration started. Without this, partial progress
                // inside the failed iteration body (especially a
                // Choice that committed before a later required item
                // failed) leaves the cursor advanced and breaks the
                // outer rule's continuation.
                if (popped.has_mark()) {
                    sr.reject();
                    popped.set_has_mark(false);
                }
                // `repeat+` (min=1) form: the Repeat itself fails if too
                // few iterations matched. Propagate the failure further up
                // so an enclosing Choice/optional can react.
                if (popped.iter_count() < popped.min()) {
                    continue;
                }
                // Iteration ended -- accept what we collected so far.
                popped.finish(pool);
                record_cache_entry(popped);
                if (stack.empty()) {
                    result_value = popped.result();
                    parse_finished = true;
                    return;
                }
                popped.pass_values_to(stack.back());
                advance_after_child();
                return;
            }
            // Sequence/Key/Parse: propagate by continuing to pop.
        }
        // Stack drained without recovery -- top-level parse fails.
    };

    push_with_optional_mark(start);
    profile_sync();

    while (!stack.empty() && !parse_finished) {
        // Pop any rule-local ignore scopes whose owning frame has been
        // popped since the previous iteration.
        trim_ignore_stack();
        profile_sync();   // catch push/pop events from the previous iteration
        if (stack.empty() || parse_finished) break;
        Frame& top = stack.back();
        switch (top.kind()) {

        case NodeKind::Raw: {
            // Raw consume: bypass the ignore-set (embedded whitespace
            // and newlines are part of the captured payload), then
            // dispatch to the unified walk_scan routine. Raw's config
            // has consume_stop=false so the stop literal is left for
            // the next sibling (a Key node) to match in its own
            // iteration.
            const Node& n = nodes_[top.node_id().value()];
            if (trace_enabled) {
                auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
                trace(std::string("  raw until \"")
                      + (sv ? sv->data() : std::string()) + "\"");
            }
            sr.mark();
            ScanConfig raw_cfg = scan_config_from_raw(n);
            raw_cfg.caller_ignore = &current_ignore();
            auto r = walk_scan(*this, sr, pool, raw_cfg);
            // Negative-lookahead inversion for Raw. Same shape as the
            // Key and Parse arms: a Raw frame marked `!*` inverts its
            // outcome before any value processing. The neg-inversion
            // happens BEFORE pool intern / subparse hook so the
            // captured string is discarded entirely on the
            // success-to-failure path (we wanted the inner to fail).
            if (top.is_negative()) {
                if (r) sr.accept(); else sr.reject();
                Frame popped = std::move(stack.back());
                stack.pop_back();
                if (popped.has_neg_mark()) {
                    sr.reject();
                    popped.set_has_neg_mark(false);
                }
                if (r) {
                    if (trace_enabled) {
                        trace("  negative-lookahead matched (failing)");
                    }
                    handle_failure(ParseError{
                        sr.position(), "negative lookahead matched"});
                } else {
                    if (trace_enabled) {
                        trace("  negative-lookahead inner failed (succeeding empty)");
                    }
                    if (!stack.empty()) advance_after_child();
                }
                break;
            }
            if (!r) {
                sr.reject();
                handle_failure(r.error());
                break;
            }
            sr.accept();
            // Raw matched — first-content guard clears.
            has_consumed_content = true;
            ValuePtr produced = *r;
            // Subparse hook: same as the Parse node branch — if this
            // Raw node carries a subparse_start, re-enter the engine
            // on the captured string with the named rule as entry.
            // The resulting sub-tree replaces the original string.
            if (n.subparse_start.valid()) {
                auto produced_sv = std::dynamic_pointer_cast<StringValue>(produced);
                if (!produced_sv) {
                    handle_failure(ParseError{
                        sr.position(),
                        "subparse requires a string-valued terminal"});
                    break;
                }
                std::istringstream sub_is(produced_sv->data());
                StreamReader sub_sr(sub_is);
                auto sub_r = parse_from(sub_sr, pool, n.subparse_start);
                if (!sub_r) {
                    handle_failure(ParseError{
                        sr.position(),
                        "subparse: " + sub_r.error().message});
                    break;
                }
                produced = *sub_r;
            }
            produced = pool.intern(produced);
            top.add_value(produced, top.is_name());
            if (top.has_current()) {
                push_or_skip_optional(top.current_child());
            } else {
                Frame popped = std::move(stack.back());
                stack.pop_back();
                // Optional frame's entry mark must be accepted so the
                // stream advance becomes permanent. Without this the
                // mark stays on the StreamReader's mark stack and the
                // NEXT mark-pop (Choice accept or any reject) targets
                // the WRONG mark — subtle and ugly.
                if (popped.is_optional() && popped.has_mark()) {
                    sr.accept();
                    popped.set_has_mark(false);
                }
                popped.finish(pool);
                record_cache_entry(popped);
                if (stack.empty()) {
                    result_value = popped.result();
                    parse_finished = true;
                    break;
                }
                popped.pass_values_to(stack.back());
                advance_after_child();
            }
            break;
        }

        case NodeKind::Scope: {
            // Bracketed-region terminal. Apply the surrounding ignore
            // set once at entry (so leading whitespace before the
            // start is tolerated), then dispatch to the unified
            // walk_scan routine. Inside the body, ignore-set skipping
            // is bypassed — embedded whitespace / comments are part
            // of the captured payload.
            run_ignore_guarded();
            const Node& n = nodes_[top.node_id().value()];
            if (trace_enabled) trace("  scope");
            sr.mark();
            ScanConfig scope_cfg = scan_config_from_scope(n);
            scope_cfg.caller_ignore = &current_ignore();
            auto r = walk_scan(*this, sr, pool, scope_cfg);
            // Negative-lookahead inversion — same shape as Key / Raw.
            if (top.is_negative()) {
                if (r) sr.accept(); else sr.reject();
                Frame popped = std::move(stack.back());
                stack.pop_back();
                if (popped.has_neg_mark()) {
                    sr.reject();
                    popped.set_has_neg_mark(false);
                }
                if (r) {
                    if (trace_enabled) {
                        trace("  negative-lookahead matched (failing)");
                    }
                    handle_failure(ParseError{
                        sr.position(), "negative lookahead matched"});
                } else {
                    if (trace_enabled) {
                        trace("  negative-lookahead inner failed (succeeding empty)");
                    }
                    if (!stack.empty()) advance_after_child();
                }
                break;
            }
            if (!r) {
                sr.reject();
                handle_failure(r.error());
                break;
            }
            sr.accept();
            // Scope matched — first-content guard clears.
            has_consumed_content = true;
            // walk_scan returns either StringValue (default string-body
            // mode) or ArrayValue (array container mode); pass through
            // verbatim. Subparse below is meaningful only for
            // string-body mode.
            ValuePtr produced = *r;
            // Subparse hook — same as Raw / Parse: if the scope node
            // carries a subparse_start, re-enter the engine on the
            // captured body with the named rule as entry.
            if (n.subparse_start.valid()) {
                auto produced_sv = std::dynamic_pointer_cast<StringValue>(produced);
                if (!produced_sv) {
                    handle_failure(ParseError{
                        sr.position(),
                        "scope subparse requires a string-valued body"});
                    break;
                }
                std::istringstream sub_is(produced_sv->data());
                StreamReader sub_sr(sub_is);
                auto sub_r = parse_from(sub_sr, pool, n.subparse_start);
                if (!sub_r) {
                    handle_failure(ParseError{
                        sr.position(),
                        "scope subparse: " + sub_r.error().message});
                    break;
                }
                produced = *sub_r;
            }
            produced = pool.intern(produced);
            top.add_value(produced, top.is_name());
            // Scope is a terminal in the Frame model — its start /
            // INNER / stop are consumed atomically by walk_scan,
            // never as Frame-iteration children. Always pop here
            // (do NOT call push_or_skip_optional on a child cursor —
            // there is no leftover child to dispatch).
            {
                Frame popped = std::move(stack.back());
                stack.pop_back();
                if (popped.is_optional() && popped.has_mark()) {
                    sr.accept();
                    popped.set_has_mark(false);
                }
                popped.finish(pool);
                record_cache_entry(popped);
                if (stack.empty()) {
                    result_value = popped.result();
                    parse_finished = true;
                    break;
                }
                popped.pass_values_to(stack.back());
                advance_after_child();
            }
            break;
        }

        case NodeKind::Key: {
            run_ignore_guarded();
            const Node& n = nodes_[top.node_id().value()];
            auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
            assert(sv);
            if (trace_enabled) trace("  key \"" + sv->data() + "\"");
            KeyParser p(sv->data(), n.strict);
            auto r = p.parse(sr);
            // Negative-lookahead inversion. A Key frame marked
            // `!"literal"` flips the success/failure of the inner
            // parse. We handle this BEFORE the normal pop path
            // because a terminal Key with no Value children pops
            // inline (does not pass through `advance_after_child`'s
            // negative-check arm at line ~1052), so the flag would
            // otherwise be silently dropped. Same inversion shape
            // as the advance_after_child arms; mirror in NodeKind::
            // Parse and NodeKind::Raw below.
            if (top.is_negative()) {
                Frame popped = std::move(stack.back());
                stack.pop_back();
                if (popped.has_neg_mark()) {
                    sr.reject();
                    popped.set_has_neg_mark(false);
                }
                if (r) {
                    if (trace_enabled) {
                        trace("  negative-lookahead matched (failing)");
                    }
                    handle_failure(ParseError{
                        sr.position(), "negative lookahead matched"});
                } else {
                    if (trace_enabled) {
                        trace("  negative-lookahead inner failed (succeeding empty)");
                    }
                    if (!stack.empty()) advance_after_child();
                }
                break;
            }
            if (r) {
                // Key matched — first-content guard clears so
                // subsequent run_ignore sites (between-items,
                // optional-boundary, end-of-parse) fire normally.
                has_consumed_content = true;
                // The literal itself is not emitted; any Value-kind
                // children, however, contribute their constants when
                // iterated.
                if (top.has_current()) {
                    push_or_skip_optional(top.current_child());
                } else {
                    Frame popped = std::move(stack.back());
                    stack.pop_back();
                    // See the Raw / Parse cases for why the optional
                    // mark accept is required here.
                    if (popped.is_optional() && popped.has_mark()) {
                        sr.accept();
                        popped.set_has_mark(false);
                    }
                    popped.finish(pool);
                    record_cache_entry(popped);
                    if (stack.empty()) {
                        result_value = popped.result();
                        parse_finished = true;
                        break;
                    }
                    popped.pass_values_to(stack.back());
                    advance_after_child();
                }
            } else {
                handle_failure(r.error());
            }
            break;
        }

        case NodeKind::Parse: {
            run_ignore_guarded();
            const Node& n = nodes_[top.node_id().value()];
            auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
            assert(sv);
            if (trace_enabled) trace("  parse:" + sv->data());
            Parser* p = parser(sv->data());
            assert(p);
            auto r = p->parse(sr);
            // Same negative-lookahead inversion as Key (see comment
            // there). Applies whether or not this Parse has Value
            // children — a terminal Parse marked `!<parser>` pops
            // inline and never reaches advance_after_child's negative
            // arm.
            if (top.is_negative()) {
                Frame popped = std::move(stack.back());
                stack.pop_back();
                if (popped.has_neg_mark()) {
                    sr.reject();
                    popped.set_has_neg_mark(false);
                }
                if (r) {
                    if (trace_enabled) {
                        trace("  negative-lookahead matched (failing)");
                    }
                    handle_failure(ParseError{
                        sr.position(), "negative lookahead matched"});
                } else {
                    if (trace_enabled) {
                        trace("  negative-lookahead inner failed (succeeding empty)");
                    }
                    if (!stack.empty()) advance_after_child();
                }
                break;
            }
            if (r) {
                // Parse matched — first-content guard clears so
                // subsequent run_ignore sites fire normally.
                has_consumed_content = true;
                ValuePtr produced = *r;
                // Subparse hook: if this Parse node carries a
                // subparse_start, re-enter the engine on the produced
                // string with the named rule as entry. Result replaces
                // the original string value. Failure propagates as a
                // parse error with the sub-position adjusted into the
                // outer stream's reference frame (best-effort: we
                // surface the inner message with a prefix and the
                // outer stream position).
                if (n.subparse_start.valid()) {
                    auto produced_sv = std::dynamic_pointer_cast<StringValue>(produced);
                    if (!produced_sv) {
                        handle_failure(ParseError{
                            sr.position(),
                            "subparse requires a string-valued terminal"});
                        break;
                    }
                    std::istringstream sub_is(produced_sv->data());
                    StreamReader sub_sr(sub_is);
                    auto sub_r = parse_from(sub_sr, pool, n.subparse_start);
                    if (!sub_r) {
                        handle_failure(ParseError{
                            sr.position(),
                            "subparse: " + sub_r.error().message});
                        break;
                    }
                    produced = *sub_r;
                }
                // Intern the produced primitive so identical content
                // (same string, same int, etc.) shares a canonical
                // ValuePtr across the entire parse.
                ValuePtr canonical = pool.intern(produced);
                top.add_value(canonical, top.is_name());
                if (top.has_current()) {
                    push_or_skip_optional(top.current_child());
                } else {
                    Frame popped = std::move(stack.back());
                    stack.pop_back();
                    // See the Raw / Key cases for why the optional
                    // mark accept is required here.
                    if (popped.is_optional() && popped.has_mark()) {
                        sr.accept();
                        popped.set_has_mark(false);
                    }
                    popped.finish(pool);
                    record_cache_entry(popped);
                    if (stack.empty()) {
                        result_value = popped.result();
                        parse_finished = true;
                        break;
                    }
                    popped.pass_values_to(stack.back());
                    advance_after_child();
                }
            } else {
                handle_failure(r.error());
            }
            break;
        }

        case NodeKind::Value: {
            // Frame ctor already pre-seeded emitted_ with this node's
            // constant (honouring is_name). Just pop and bubble up.
            Frame popped = std::move(stack.back());
            stack.pop_back();
            popped.finish(pool);
            record_cache_entry(popped);
            if (stack.empty()) {
                result_value = popped.result();
                parse_finished = true;
                break;
            }
            popped.pass_values_to(stack.back());
            advance_after_child();
            break;
        }

        case NodeKind::Choice: {
            // Peek-and-skip alts whose first-byte set can't include
            // the next input byte. Steps past doomed alts in O(1)
            // each — for LAYER_KEYWORD (46 alts) and similar
            // closed-keyword choices, this collapses a 97%
            // try-and-rewind workload into a single peek + lookup.
            while (top.has_current()
                   && choice_alt_cant_match(top.current_child())) {
                top.step_next();
            }
            if (top.has_current()) {
                // Opt-in: if this Choice is marked as backtracking and we
                // haven't yet issued a stream mark for the current
                // alternative attempt, mark now. The mark will be
                // accepted (on alternative success) or rejected (on
                // alternative failure) before we move on.
                if (top.is_backtrack() && !top.has_mark()) {
                    sr.mark();
                    top.set_has_mark(true);
                }
                push_or_skip_optional(top.current_child());
            } else if (stack.size() > 1) {
                // Every alt's first-byte set excluded the input. The
                // Choice has nothing more to try; trigger the same
                // failure-recovery path the normal "Choice exhausted"
                // unwind takes. handle_failure walks back to the
                // nearest recoverable level (outer Choice with more
                // alts, or optional, or repeat).
                handle_failure(ParseError{
                    sr.position(),
                    "no choice alternative matched input first byte"});
            } else {
                Frame popped = std::move(stack.back());
                stack.pop_back();
                popped.finish(pool);
                record_cache_entry(popped);
                if (stack.empty()) {
                    result_value = popped.result();
                    parse_finished = true;
                    break;
                }
                popped.pass_values_to(stack.back());
                advance_after_child();
            }
            break;
        }

        case NodeKind::Repeat: {
            if (top.has_current()) {
                // Mark the start of each iteration so a failed body
                // can roll back cleanly. Standard PEG repeat
                // semantics: partial progress in a failed iteration
                // must not affect the outer rule. Without this, a
                // Choice inside the iteration body that committed
                // (accepted its own mark) would leave the cursor
                // advanced when a later required item in the body
                // failed — the surrounding rule would then fail too,
                // unable to roll back over the committed Choice.
                if (!top.has_mark()) {
                    sr.mark();
                    top.set_has_mark(true);
                }
                push_or_skip_optional(top.current_child());
            } else {
                // No children left to process -- this frame is done.
                Frame popped = std::move(stack.back());
                stack.pop_back();
                popped.finish(pool);
                record_cache_entry(popped);
                if (stack.empty()) {
                    result_value = popped.result();
                    parse_finished = true;
                    break;
                }
                popped.pass_values_to(stack.back());
                advance_after_child();
            }
            break;
        }

        case NodeKind::Sequence: {
            if (top.has_current()) {
                push_or_skip_optional(top.current_child());
            } else {
                // No children left to process -- this frame is done.
                Frame popped = std::move(stack.back());
                stack.pop_back();
                popped.finish(pool);
                record_cache_entry(popped);
                if (stack.empty()) {
                    result_value = popped.result();
                    parse_finished = true;
                    break;
                }
                popped.pass_values_to(stack.back());
                advance_after_child();
            }
            break;
        }

        default:
            // Ref resolved at push time; should never reach a top frame.
            assert(false && "unreachable: Ref at top of parse stack");
            handle_failure(ParseError{sr.position(), "internal: unexpected node kind"});
            break;
        }
    }

    // Drain any remaining frames in the profile counters (e.g. the
    // start frame's elapsed time gets added on the final iteration),
    // then build the public report.
    profile_sync();
    if (profile_enabled) {
        last_profile_report_ = {};
        last_profile_report_.total_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - profile_start_clock).count();
        last_profile_report_.total_frames = profile_total_frames;
        last_profile_report_.max_depth    = profile_max_depth;
        for (std::size_t i = 0; i < profile_per_node.size(); ++i) {
            if (profile_per_node[i].entry_count == 0) continue;
            // Look up the rule name (if this NodeId is a named rule's
            // body). Anonymous (inline) nodes keep an empty rule_name.
            for (const auto& [name, rid] : named_rules_) {
                if (rid.value() == i) {
                    profile_per_node[i].rule_name = name;
                    break;
                }
            }
            last_profile_report_.entries.push_back(std::move(profile_per_node[i]));
        }
    }

    if (parse_finished) {
        // Start rule produced a complete value. For top-level / subparse
        // callers we require the rest of the stream (modulo trailing
        // ignored terminals) to be empty — without this check a grammar
        // that matches a prefix would silently succeed, hiding coverage
        // gaps when the file has unmodeled content after the matched
        // portion. Sub-invocations from byte-scan INNER trials disable
        // the check via `require_full_consume=false`; they legitimately
        // stop at the boundary the rule chose, and crucially MUST NOT
        // eat trailing ignore bytes — those bytes belong to the
        // surrounding scope's body capture, not to the INNER.
        if (require_full_consume) {
            run_ignore(sr, current_ignore());
            if (!sr.eof()) {
                // Surface the deepest position the engine reached
                // INSIDE failed sub-parses if it's beyond where the
                // start rule stopped. This catches the common shape
                // where a repeat-zero-or-more wraps a Choice that
                // genuinely tried (and failed deep) to parse a
                // construct — the top-level "stopped here" position
                // is misleading; the real gap is further in.
                std::string msg = "unexpected content after start "
                                  "rule completed";
                if (max_progress.position.bytes > sr.position().bytes) {
                    msg += " — deepest failed sub-parse reached byte "
                         + std::to_string(max_progress.position.bytes)
                         + " (line "
                         + std::to_string(max_progress.position.line)
                         + ", column "
                         + std::to_string(max_progress.position.column)
                         + "): "
                         + max_progress.message;
                }
                return tl::unexpected(ParseError{sr.position(), std::move(msg)});
            }
        }
        // `#opchain` post-process: when the start rule body (or any
        // Ref along the chain to it) carries the flag, compact any
        // always-wrap chain shape in the produced AST. See
        // `compact_opchain` for the transform.
        if (has_opchain_in_chain(start) || has_any_opchain()) {
            result_value = compact_opchain(result_value);
        }
        return result_value;
    }
    return tl::unexpected(max_progress);
}


// Grammar::save is defined in src/save_stack.cpp so the save
// engine (helpers + entry point) stays in one file. See the
// stack-navigation walk for how the implementation handles
// fixed/open-schema dicts, key-based Choice dispatch, wrapped
// sub-structures, catch-all alternatives, and the self-host case.

// -------------------------------------------------------------------------
// JSON grammar — first working grammar, built in code
// -------------------------------------------------------------------------

Grammar make_json_grammar() {
    Grammar g;

    // The in-memory JSON grammar is JSONC: it tolerates // line and
    // /* block */ comments anywhere whitespace is allowed. This is a
    // pragmatic default for the host-API grammar used to bootstrap
    // JSON-form grammar files (which typically carry inline docs).
    // File-loaded grammars opt into JSONC explicitly via their
    // top-level "ignore" field.
    //
    // Terminal parsers come from the `std` parser group, registered
    // here through the same mechanism a file-loaded grammar uses via
    // `"use": ["std"]`. Both paths converge on the same parser set.
    register_std_parser_group();
    auto r = apply_parser_group(g, "std");
    (void)r;   // std group always present after the call above
    g.add_ignore("whitespace");
    g.add_ignore("line_comment");
    g.add_ignore("block_comment");

    // VALUE: choice of primitives and containers.
    NodeId value = g.new_choice();
    g.register_rule("VALUE", value);
    g.add_ref(value, "STRUCT");
    g.add_ref(value, "LIST");
    g.add_parse(value, "string");
    // float must come before int because float requires '.' or 'e' so a
    // bare integer falls through to the int branch; the order here is the
    // predictive-PEG ordering described in §2.4.
    g.add_parse(value, "float");
    g.add_parse(value, "int");

    NodeId k_null  = g.add_key(value, "null");
    g.add_value(k_null, null_value());
    NodeId k_true  = g.add_key(value, "true");
    g.add_value(k_true, true_value());
    NodeId k_false = g.add_key(value, "false");
    g.add_value(k_false, false_value());

    // LIST: [ item, item, ... ]
    NodeId list = g.new_sequence();
    g.register_rule("LIST", list);
    g.set_container(list, Container::Array);
    g.add_key(list, "[");
    NodeId list_rep = g.add_repeat(list);
    g.add_key(list, "]");
    g.set_separator(list_rep, g.new_key(","));
    g.add_ref(list_rep, "VALUE");

    // STRUCT: { "key": value, ... }
    NodeId structure = g.new_sequence();
    g.register_rule("STRUCT", structure);
    g.set_container(structure, Container::Dict);
    g.add_key(structure, "{");
    NodeId struct_rep = g.add_repeat(structure);
    g.add_key(structure, "}");
    g.set_separator(struct_rep, g.new_key(","));
    NodeId pair = g.add_sequence(struct_rep);
    NodeId name = g.add_parse(pair, "string");
    g.set_name(name);
    g.add_key(pair, ":");
    g.add_ref(pair, "VALUE");

    g.set_top(g.new_ref("VALUE"));

    return g;
}

} // namespace rawast
