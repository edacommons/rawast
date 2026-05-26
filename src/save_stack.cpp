// Phase B save engine — stack-navigation rewrite. See save_stack.hpp.
//
// Replaces the flat-cursor save with a two-pointer walk:
//
//   * A QUEUE of pending values (positional flow — for Sequences
//     container=None, Array iterations, open-schema dict flatten).
//   * A DICT SCOPE STACK — the fixed-schema dict currently being
//     filled. Value-name marker children look up by name in the top
//     dict scope; this is the transitive descent that makes optional
//     nested sub-structures (CONTAINER_OPT, BIND_TAIL_OPT) work.
//   * A PENDING-NAME slot — set when crossing a Value-name marker;
//     consumed by the next consumer child via pull_value().
//
// Same Grammar drives both directions. Same Pretty-print attributes
// (head/tail/depth_in/indent_emit/space_after/newline_after).

#include "save_stack.hpp"

#include <rawast/grammar.hpp>
#include <rawast/node.hpp>
#include <rawast/parser.hpp>
#include <rawast/parsers.hpp>
#include <rawast/value.hpp>

#include <cassert>
#include <memory>
#include <optional>
#include <ostream>
#include <unordered_set>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rawast {

namespace {

// ---------------------------------------------------------------------------
// SaveState
// ---------------------------------------------------------------------------

struct QueueEntry {
    ValuePtr    value;
    bool        is_name = false;
    // When this entry was produced by an open-schema dict flatten,
    // `key_source` carries the originating dict key so Choice dispatch
    // can match it as a literal Key token.
    std::string key_source;
};

struct DictScope {
    const DictValue* dict;
    std::set<std::string> consumed;
};

class SaveState {
public:
    // --- queue ---
    void push_q(QueueEntry e) { queue_.push_back(std::move(e)); }
    bool has_q() const { return q_idx_ < queue_.size(); }
    const QueueEntry& peek_q() const { return queue_[q_idx_]; }
    QueueEntry next_q() { return std::move(queue_[q_idx_++]); }

    // --- dict scope ---
    void push_dict(const DictValue& d) { dict_stack_.push_back({&d, {}}); }
    void pop_dict() { dict_stack_.pop_back(); }
    DictScope* top_dict() {
        return dict_stack_.empty() ? nullptr : &dict_stack_.back();
    }
    const DictScope* top_dict() const {
        return dict_stack_.empty() ? nullptr : &dict_stack_.back();
    }

    // --- pending name marker ---
    void set_pending(std::string name) {
        pending_ = std::move(name);
        have_pending_ = true;
    }
    bool has_pending() const { return have_pending_; }
    const std::string& pending_name() const { return pending_; }
    void clear_pending() { have_pending_ = false; pending_.clear(); }

    // Snapshot/restore for try-then-commit (optional sequences,
    // alternative selection). Stores indices/sizes — fast, no copies.
    struct Snapshot {
        std::size_t queue_size;
        std::size_t q_idx;
        std::size_t dict_stack_size;
        std::string pending;
        bool have_pending;
    };
    Snapshot snapshot() const {
        return {queue_.size(), q_idx_, dict_stack_.size(), pending_, have_pending_};
    }
    void restore(const Snapshot& s) {
        queue_.resize(s.queue_size);
        q_idx_ = s.q_idx;
        dict_stack_.resize(s.dict_stack_size);
        pending_ = s.pending;
        have_pending_ = s.have_pending;
    }

    // Combined value pull: prefer pending-name lookup in the top dict
    // scope; fall back to queue. Returns null QueueEntry on:
    //   * absent-and-optional pending field, or
    //   * empty queue while in a dict scope (NO_DATA-style consumer:
    //     a Parse without a preceding name marker, e.g. gds_endlib).
    // Errors on absent-required pending or empty queue outside any
    // dict scope.
    tl::expected<QueueEntry, SaveError> pull_value(bool is_optional) {
        if (have_pending_ && !dict_stack_.empty()) {
            auto& scope = dict_stack_.back();
            const std::string name = pending_;
            clear_pending();
            auto it = scope.dict->data().find(name);
            if (it != scope.dict->data().end()) {
                scope.consumed.insert(name);
                return QueueEntry{it->second, false, name};
            }
            if (is_optional) {
                return QueueEntry{nullptr, false, name};
            }
            return tl::unexpected(SaveError{
                "fixed-schema dict missing required field '" + name + "'"});
        }
        if (have_pending_) {
            // Pending set but no dict scope — treat as positional and
            // fall through to the queue.
            clear_pending();
        }
        if (!has_q()) {
            // Empty queue. In a dict scope this is a NO_DATA-style
            // consumer (e.g. gds_endlib's structural Parse without a
            // preceding name marker) — pass null and let the parser's
            // unparse decide. Outside a dict scope, only optional
            // consumers tolerate absence.
            if (!dict_stack_.empty() || is_optional) {
                return QueueEntry{nullptr, false, ""};
            }
            return tl::unexpected(SaveError{
                "save: value queue empty when value expected"});
        }
        return next_q();
    }

private:
    std::vector<QueueEntry> queue_;
    std::size_t             q_idx_ = 0;
    std::vector<DictScope>  dict_stack_;
    std::string             pending_;
    bool                    have_pending_ = false;
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

bool can_consume(const Grammar& g, NodeId node_id, const SaveState& s);

tl::expected<void, SaveError>
do_consume(const Grammar& g, std::ostream& out, NodeId node_id,
           SaveState& s, int depth, bool pretty);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Does this Sequence behave as a fixed-schema dict (push scope, look up
// fields by name) or an open-schema dict (flatten entries into queue,
// iterate)?
//
// Heuristic:
//   * Any direct Value-name child → fixed-schema (explicit named field).
//   * Any direct Ref child        → fixed-schema (specific sub-structure;
//     its V-names live one indirection away and would not be visible by
//     a shallow check, but its presence signals "structured content").
//   * Else (only Repeats and structural Keys)  → open-schema (the body
//     is "iterate this dict's entries"; FILE-style).
//
// Replaces the older explicit `fixed_schema: true` flag — the grammar
// no longer needs to tell the engine which mode to use.
bool has_name_markers(const Grammar& g, const Node& seq) {
    for (NodeId c : seq.children) {
        // Original child (Ref check must NOT resolve — the resolved
        // target's kind is the rule's body kind, not Ref).
        const Node& orig = g.node(c);
        if (orig.kind == NodeKind::Ref) return true;
        // V-name marker direct child.
        const Node& cn = g.node(g.resolve_ref(c));
        if (cn.kind == NodeKind::Value && cn.is_name) return true;
    }
    return false;
}

// Follow a Ref chain checking `is_optional` on each node along the
// way. Used at sequence-walk time to detect ref-site optional patterns
// like `?<RULE>` or `?gds_reflibs` where the consumer itself doesn't
// carry the flag.
bool node_is_optional_chain(const Grammar& g, NodeId id) {
    NodeId cur = id;
    while (cur.valid()) {
        const Node& cn = g.node(cur);
        if (cn.is_optional) return true;
        if (cn.kind != NodeKind::Ref) break;
        auto sv = std::dynamic_pointer_cast<StringValue>(cn.value);
        if (!sv) break;
        cur = g.rule_id(sv->data());
    }
    return false;
}

// Pre-check: would this child be skipped because its pending name has
// no value in the current dict scope AND it's marked optional?
bool would_skip_optional(const Grammar& g, NodeId child_id, const SaveState& s) {
    if (!s.has_pending() || !s.top_dict()) return false;
    if (!node_is_optional_chain(g, child_id)) return false;
    const auto& d = s.top_dict()->dict->data();
    return d.find(s.pending_name()) == d.end();
}

// Extract the constant value bound to a Value-name by the following
// child. Two forms recognised:
//   1. Value-const sibling — `{"type":"value","value":"optional"}`
//      (the OPTIONAL_EXPR pattern).
//   2. Key with a Value-kind child — `{"type":"key","key":"sequence",
//      "value":"sequence"}` (the SEQUENCE_EXPR pattern).
// Returns nullptr if `b` is neither shape.
ValuePtr discriminator_const_value(const Grammar& g, const Node& b) {
    if (b.kind == NodeKind::Value && !b.is_name) return b.value;
    if (b.kind == NodeKind::Key) {
        for (NodeId kc : b.children) {
            const Node& kcn = g.node(g.resolve_ref(kc));
            if (kcn.kind == NodeKind::Value && kcn.value) return kcn.value;
        }
    }
    return nullptr;
}

// Does this Choice alternative have any explicit discriminator?
// Explicit discriminator forms recognised:
//   * `(Value-name X, Value-const C)` adjacent — dict[X]==C must hold.
//   * `(Value-name X, Key K with Value-child C)` adjacent — same.
//   * First-child Key with literal token — key-based dispatch.
// Catch-all alternatives have none of these and are tried last.
bool has_explicit_discriminator(const Grammar& g, NodeId alt_id) {
    NodeId rid = g.resolve_ref(alt_id);
    const Node& n = g.node(rid);
    if (n.kind == NodeKind::Sequence) {
        for (std::size_t i = 0; i + 1 < n.children.size(); ++i) {
            const Node& a = g.node(g.resolve_ref(n.children[i]));
            const Node& b = g.node(g.resolve_ref(n.children[i + 1]));
            if (a.kind != NodeKind::Value || !a.is_name) continue;
            if (discriminator_const_value(g, b)) return true;
        }
        if (!n.children.empty()) {
            const Node& first = g.node(g.resolve_ref(n.children[0]));
            if (first.kind == NodeKind::Key && first.value) return true;
        }
        return false;
    }
    if (n.kind == NodeKind::Key) return true;
    return false;
}

// Compare two ValuePtrs for save-direction equality. Pointer identity
// is the fast path (constants in the pool); fall back to type+payload.
bool values_equal_v2(const ValuePtr& a, const Value* b) {
    if (a.get() == b) return true;
    if (!a || !b) return false;
    if (a->type() != b->type()) return false;
    switch (a->type()) {
    case ValueType::Null:    return true;
    case ValueType::Bool:    return std::static_pointer_cast<BoolValue>(a)->data()
                                   == static_cast<const BoolValue*>(b)->data();
    case ValueType::Int:     return std::static_pointer_cast<IntValue>(a)->data()
                                   == static_cast<const IntValue*>(b)->data();
    case ValueType::UInt:    return std::static_pointer_cast<UIntValue>(a)->data()
                                   == static_cast<const UIntValue*>(b)->data();
    case ValueType::Real:    return std::static_pointer_cast<RealValue>(a)->data()
                                   == static_cast<const RealValue*>(b)->data();
    case ValueType::String:  return std::static_pointer_cast<StringValue>(a)->data()
                                   == static_cast<const StringValue*>(b)->data();
    default: return false;   // arrays / dicts: only pointer-equal counts
    }
}

// ---------------------------------------------------------------------------
// can_consume — pure check used by Choice dispatch
// ---------------------------------------------------------------------------

// Walk a candidate alternative against the current state and return
// whether it would commit successfully. No side effects on `s`.
//
// Strategy: peek at the value that would flow into the alt's first
// consumer position and check whether it satisfies the alt's shape:
//
//   * Bare Key first child: match if value is a StringValue equal to
//     the Key's token (key-based dispatch).
//   * Discriminator pair (Value-name + Value-const): require the
//     containing dict to have name=const.
//   * Container=Dict sequence: require value type is dict; if it
//     has discriminators they must match.
//   * Container=None / Parse first child: type-compatible value.
// Forward declaration: variant that takes an explicit peek_value (used by
// the container=None deep walk when simulating pending-name consumption).
bool can_consume_peek(const Grammar& g, NodeId node_id,
                      ValuePtr peek_value, const std::string& peek_key,
                      const SaveState& s);

bool can_consume(const Grammar& g, NodeId node_id, const SaveState& s) {
    // What value would the alt see? Either the pending-name lookup
    // (top dict scope) or the next queue entry.
    ValuePtr peek_value;
    std::string peek_key;
    if (s.has_pending() && s.top_dict()) {
        auto it = s.top_dict()->dict->data().find(s.pending_name());
        if (it != s.top_dict()->dict->data().end()) {
            peek_value = it->second;
            peek_key   = s.pending_name();
        }
    } else if (s.has_q()) {
        peek_value = s.peek_q().value;
        peek_key   = s.peek_q().key_source;
    }
    return can_consume_peek(g, node_id, peek_value, peek_key, s);
}

bool can_consume_peek(const Grammar& g, NodeId node_id,
                      ValuePtr peek_value, const std::string& peek_key,
                      const SaveState& s) {
    NodeId rid = g.resolve_ref(node_id);
    const Node& n = g.node(rid);

    switch (n.kind) {
    case NodeKind::Key: {
        // Discriminator Key (has Value-children): match if peek value
        // equals a Value-child constant.
        for (NodeId c : n.children) {
            const Node& cn = g.node(g.resolve_ref(c));
            if (cn.kind == NodeKind::Value && cn.value && peek_value
                && values_equal_v2(peek_value, cn.value.get())) {
                return true;
            }
        }
        // Bare Key: key-based dispatch. Match if peek value is a
        // StringValue equal to this Key's literal token.
        if (n.value && peek_value) {
            auto tok = std::dynamic_pointer_cast<StringValue>(n.value);
            auto vs  = std::dynamic_pointer_cast<StringValue>(peek_value);
            if (tok && vs && tok->data() == vs->data()) return true;
        }
        // Match against the originating dict key (open-schema flatten).
        if (n.value && !peek_key.empty()) {
            auto tok = std::dynamic_pointer_cast<StringValue>(n.value);
            if (tok && tok->data() == peek_key) return true;
        }
        return false;
    }

    case NodeKind::Parse: {
        if (!peek_value) return false;
        auto name = std::dynamic_pointer_cast<StringValue>(n.value);
        if (!name) return false;
        const std::string& p = name->data();
        ValueType vt = peek_value->type();

        // Standard parsers: strict value-type match. Order is
        // important — `int`/`uint` is checked before the generic
        // custom-parser fallback so `{"type":"string"}` doesn't claim
        // to handle an IntValue.
        if (p == "string") return vt == ValueType::String;
        if (p == "identifier") {
            // Reject strings the IdentifierParser wouldn't accept on
            // re-parse (e.g. dotted names like "gdsii.header"). Lets
            // Choice dispatch pick a `qualified_identifier` catch-all
            // alt when the value is a dotted name. The check matches
            // IdentifierParser's default char set: [A-Za-z_][A-Za-z0-9_]*.
            if (vt != ValueType::String) return false;
            const auto& s =
                std::static_pointer_cast<StringValue>(peek_value)->data();
            if (s.empty()) return false;
            auto is_lead = [](char c) {
                return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
            };
            auto is_cont = [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
            };
            if (!is_lead(s[0])) return false;
            for (std::size_t i = 1; i < s.size(); ++i) {
                if (!is_cont(s[i])) return false;
            }
            return true;
        }
        if (p == "qualified_identifier") return vt == ValueType::String;
        if (p == "int" || p == "uint")
            return vt == ValueType::Int || vt == ValueType::UInt;
        if (p == "float") return vt == ValueType::Real;
        if (p == "whitespace" || p == "line_comment" || p == "block_comment")
            return false;   // ignore-list parsers don't dispatch values
        // Custom parser (e.g. gds_header): registered name implies
        // the parser knows how to unparse — concrete type compat is
        // checked at unparse time.
        return g.parser(p) != nullptr;
    }

    case NodeKind::Sequence: {
        if (n.container == Container::Array) {
            return peek_value && peek_value->type() == ValueType::Array;
        }
        if (n.container == Container::Dict) {
            if (!peek_value || peek_value->type() != ValueType::Dict) return false;
            // Check (Value-name, Value-const | Key-with-Value-child)
            // discriminator pairs — all must match. If no explicit
            // discriminator exists, this alt is a dict-shaped catch-all
            // (matches any dict).
            const auto* dict = dynamic_cast<const DictValue*>(peek_value.get());
            for (std::size_t i = 0; i + 1 < n.children.size(); ++i) {
                const Node& a = g.node(g.resolve_ref(n.children[i]));
                const Node& b = g.node(g.resolve_ref(n.children[i + 1]));
                if (a.kind != NodeKind::Value || !a.is_name) continue;
                auto name_sv = std::dynamic_pointer_cast<StringValue>(a.value);
                if (!name_sv) continue;
                ValuePtr cv = discriminator_const_value(g, b);
                if (!cv) continue;
                auto it = dict->data().find(name_sv->data());
                if (it == dict->data().end()) return false;
                if (!values_equal_v2(it->second, cv.get())) return false;
            }
            return true;
        }
        // container=None: prefer discriminator pairs against the
        // surrounding dict scope when available. Falls back to checking
        // the first non-structural consumer child.
        if (const DictScope* scope = s.top_dict()) {
            bool found_disc = false;
            for (std::size_t i = 0; i + 1 < n.children.size(); ++i) {
                const Node& a = g.node(g.resolve_ref(n.children[i]));
                const Node& b = g.node(g.resolve_ref(n.children[i + 1]));
                if (a.kind != NodeKind::Value || !a.is_name) continue;
                auto name_sv = std::dynamic_pointer_cast<StringValue>(a.value);
                if (!name_sv) continue;
                ValuePtr cv = discriminator_const_value(g, b);
                if (!cv) continue;
                auto it = scope->dict->data().find(name_sv->data());
                if (it == scope->dict->data().end()) return false;
                if (!values_equal_v2(it->second, cv.get())) return false;
                found_disc = true;
            }
            if (found_disc) return true;
        }
        // Field-presence walk: when no Value-const discriminator matched,
        // simulate Value-name markers updating pending, and require
        // dict-has-field for each real consumer that follows a pending
        // name marker. Lets alts whose only discriminator is "this
        // field must exist" (rather than "this field must equal a
        // constant") still self-discriminate during Choice dispatch.
        if (const DictScope* scope = s.top_dict()) {
            std::string sim_pending;
            bool have_sim_pending = false;
            bool walked_consumer = false;
            for (NodeId c : n.children) {
                const Node& cn = g.node(g.resolve_ref(c));
                if (cn.kind == NodeKind::Value && cn.is_name) {
                    if (auto sv = std::dynamic_pointer_cast<StringValue>(cn.value)) {
                        sim_pending = sv->data();
                        have_sim_pending = true;
                    }
                    continue;
                }
                if (cn.kind == NodeKind::Value) continue;
                if (cn.kind == NodeKind::Key) continue;   // structural

                // Real consumer.
                if (have_sim_pending) {
                    auto it = scope->dict->data().find(sim_pending);
                    if (it == scope->dict->data().end()) return false;
                    // NSEQ semantics: reject alts whose required field
                    // has already been consumed by a sibling iteration.
                    // Lets `repeat <choice>` iterate dict fields one at
                    // a time without picking the same alt twice.
                    if (scope->consumed.count(sim_pending)) return false;
                    if (!can_consume_peek(g, c, it->second, sim_pending, s))
                        return false;
                    have_sim_pending = false;
                    walked_consumer = true;
                }
            }
            if (walked_consumer) return true;
        }
        // Walk children. Logic:
        //   * Value (names + constants) — skip.
        //   * Key with Value-child — discriminator; dispatch via the
        //     Key's can_consume (its Value-child is the constant).
        //   * Bare Key whose literal matches peek_value / peek_key —
        //     key-based dispatch (USE_DECL pattern). Return true.
        //   * Other bare Keys ("<", "{", ":") — structural; continue.
        //   * Any other kind — first real consumer; return its
        //     can_consume against the current state.
        for (NodeId c : n.children) {
            const Node& cn = g.node(g.resolve_ref(c));
            if (cn.kind == NodeKind::Value) continue;
            if (cn.kind == NodeKind::Key) {
                bool has_value_child = false;
                for (NodeId kc : cn.children) {
                    if (g.node(g.resolve_ref(kc)).kind == NodeKind::Value) {
                        has_value_child = true; break;
                    }
                }
                if (has_value_child) {
                    return can_consume_peek(g, c, peek_value, peek_key, s);
                }
                if (cn.value) {
                    auto tok = std::dynamic_pointer_cast<StringValue>(cn.value);
                    if (tok) {
                        auto vs = std::dynamic_pointer_cast<StringValue>(peek_value);
                        if (vs && vs->data() == tok->data()) return true;
                        if (!peek_key.empty() && peek_key == tok->data()) return true;
                    }
                }
                continue;
            }
            return can_consume_peek(g, c, peek_value, peek_key, s);
        }
        return false;
    }

    case NodeKind::Choice:
    case NodeKind::Repeat:
        // Choice / Repeat at top of an alt — assume they would dispatch.
        return peek_value != nullptr;

    case NodeKind::Value:
        // Used as a child during walks, not as a dispatch root.
        return true;

    case NodeKind::Ref:
        return can_consume_peek(g, g.resolve_ref(node_id), peek_value, peek_key, s);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Choice dispatch
// ---------------------------------------------------------------------------

NodeId choose_alternative_v2(const Grammar& g, const Node& choice_node,
                              const SaveState& s) {
    // First pass: try alternatives with explicit discriminators.
    for (NodeId alt : choice_node.children) {
        if (!has_explicit_discriminator(g, alt)) continue;
        if (can_consume(g, alt, s)) return alt;
    }
    // Second pass: catch-all alternatives (no explicit discriminator).
    for (NodeId alt : choice_node.children) {
        if (has_explicit_discriminator(g, alt)) continue;
        if (can_consume(g, alt, s)) return alt;
    }
    return NodeId{};
}

// ---------------------------------------------------------------------------
// do_consume — emission + state advance
// ---------------------------------------------------------------------------

// Body without pretty-print wrapping (head/tail/indent applied by caller).
tl::expected<void, SaveError>
do_consume_body(const Grammar& g, std::ostream& out, NodeId node_id,
                SaveState& s, int depth, bool pretty) {
    NodeId rid = g.resolve_ref(node_id);
    const Node& n = g.node(rid);

    switch (n.kind) {
    case NodeKind::Ref:
        // Already resolved.
        return tl::unexpected(SaveError{"internal: unresolved Ref at save_v2"});

    case NodeKind::Value: {
        if (n.is_name) {
            // Name marker — set pending; next consumer looks it up.
            auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
            if (sv) s.set_pending(sv->data());
        } else {
            // Constant — closes a (Value-name, Value-const) discriminator
            // pair. Look up dict[pending] in the current scope:
            //   * matches const  → pair satisfied, clear pending.
            //   * mismatch       → leave pending; a following optional
            //     block is expected to emit the override (ITEM's
            //     `type=bare` default + BIND_TAIL_OPT override pattern).
            //   * no dict scope  → no save-time effect (Value-const in
            //     container=None alternatives like REF dispatch).
            if (s.has_pending() && s.top_dict()) {
                const auto& d = s.top_dict()->dict->data();
                auto it = d.find(s.pending_name());
                if (it != d.end() && values_equal_v2(it->second, n.value.get())) {
                    s.clear_pending();
                }
            }
        }
        return {};
    }

    case NodeKind::Key: {
        auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
        if (!sv) return tl::unexpected(SaveError{"Key without literal token"});

        // Does this Key consume a value? Either: it has a Value-child
        // (discriminator emit), OR a pending name expects to consume
        // from the dict scope (the Key acts as the consumer for the
        // name-marker), OR there's a key_source matching the literal
        // (open-schema dict iteration with key-based dispatch).
        bool has_value_child = false;
        for (NodeId c : n.children) {
            const Node& cn = g.node(g.resolve_ref(c));
            if (cn.kind == NodeKind::Value) {
                has_value_child = true;
                break;
            }
        }

        if (has_value_child) {
            // Consume one value (pending lookup or queue).
            auto r = s.pull_value(/*is_optional=*/false);
            if (!r) return tl::unexpected(r.error());
        } else if (s.has_q() && !s.peek_q().key_source.empty() &&
                   s.peek_q().key_source == sv->data()) {
            // Open-schema key-based dispatch: this Key represents the
            // current dict key; consume the (name, value) pair's name
            // entry. The value entry will be consumed by the next
            // consumer in the alt.
            s.next_q();
        }
        // Always emit the literal text.
        out << sv->data();
        return {};
    }

    case NodeKind::Parse: {
        auto name = std::dynamic_pointer_cast<StringValue>(n.value);
        if (!name) return tl::unexpected(SaveError{"Parse node without parser name"});
        Parser* p = g.parser(name->data());
        if (!p) return tl::unexpected(SaveError{"unknown parser: " + name->data()});

        // Pull a value via the dict-scope-aware path. Optional-chain
        // checks happen upstream (would_skip_optional); by the time
        // we reach here, we either have a value, a NO_DATA placeholder
        // (null), or the caller wants this Parse marked optional.
        const bool is_optional = n.is_optional;
        auto r = s.pull_value(is_optional);
        if (!r) return tl::unexpected(r.error());
        ValuePtr v = r->value ? r->value : null_value();
        auto u = p->unparse(*v);
        if (!u) return tl::unexpected(u.error());
        out << *u;
        return {};
    }

    case NodeKind::Sequence: {
        if (n.container == Container::None) {
            // Walk children in order with the same state.
            // Optional sequences: use can_consume as a read-only check
            // — if any consumer would fail to find its data, skip the
            // whole block silently; otherwise commit. Two-pass design
            // (check then write) avoids the state-restore bugs of an
            // earlier trial-output approach.
            //
            // Use-site optional (e.g. `?<RULE>`) lives on the Ref node,
            // not the resolved rule. Walk the Ref chain to detect both.
            if (n.is_optional || node_is_optional_chain(g, node_id)) {
                // Read-only check: if the wrapper's bindings + consumers
                // can't all match the current state, skip silently.
                if (!can_consume(g, rid, s)) {
                    s.clear_pending();
                    return {};
                }
                for (NodeId c : n.children) {
                    auto r = do_consume(g, out, c, s, depth, pretty);
                    if (!r) return r;
                }
                return {};
            }
            for (NodeId c : n.children) {
                // Optional-field skip: if this child is marked optional
                // (or refs into one) AND a pending name is set whose
                // value is absent from the current dict scope, skip
                // both the child AND the pending name (the optional
                // section opts out).
                if (would_skip_optional(g, c, s)) {
                    s.clear_pending();
                    continue;
                }
                auto r = do_consume(g, out, c, s, depth, pretty);
                if (!r) return r;
            }
            return {};
        }

        // Container=Dict / Container=Array: consume one value from
        // outer state, then dive in with a fresh substate.
        auto pulled = s.pull_value(/*is_optional=*/false);
        if (!pulled) return tl::unexpected(pulled.error());
        ValuePtr v = pulled->value;
        if (!v) return tl::unexpected(SaveError{
            "container Sequence with null value"});

        if (n.container == Container::Array) {
            auto arr = std::dynamic_pointer_cast<ArrayValue>(v);
            if (!arr) return tl::unexpected(SaveError{
                "expected ArrayValue for container=Array sequence"});

            SaveState inner;
            for (const auto& e : arr->data()) {
                inner.push_q({e, false, ""});
            }
            for (NodeId c : n.children) {
                auto r = do_consume(g, out, c, inner, depth, pretty);
                if (!r) return r;
            }
            return {};
        }

        // container=Dict
        auto dict = std::dynamic_pointer_cast<DictValue>(v);
        if (!dict) return tl::unexpected(SaveError{
            "expected DictValue for container=Dict sequence"});

        if (has_name_markers(g, n) || n.fixed_schema) {
            // Fixed-schema: walk children with the dict pushed as a
            // scope. Value-name markers update pending; consumers
            // pull from the dict by name. The `fixed_schema` flag is
            // an explicit override for cases where the named fields
            // live inside nested rules (e.g. via a Ref/Choice) and
            // `has_name_markers` can't see them.
            SaveState inner;
            inner.push_dict(*dict);
            for (NodeId c : n.children) {
                if (would_skip_optional(g, c, inner)) {
                    inner.clear_pending();
                    continue;
                }
                auto r = do_consume(g, out, c, inner, depth, pretty);
                if (!r) return r;
            }
            inner.pop_dict();
            return {};
        }

        // Open-schema: flatten alphabetically as (key-as-name, value)
        // queue entries; the inner Repeat/Choice iterates them. Keep
        // the key_source so Choice dispatch can match Key literals.
        SaveState inner;
        for (const auto& [name, val] : dict->data()) {
            inner.push_q({make_string(name), true, name});
            inner.push_q({val, false, name});
        }
        for (NodeId c : n.children) {
            auto r = do_consume(g, out, c, inner, depth, pretty);
            if (!r) return r;
        }
        return {};
    }

    case NodeKind::Repeat: {
        const bool has_sep = n.has_separator && n.children.size() >= 2;
        NodeId sep_id  = has_sep ? n.children[0] : NodeId{};
        NodeId item_id = has_sep ? n.children[1]
                                  : (n.children.empty() ? NodeId{}
                                                        : n.children[0]);
        if (!item_id.valid()) return {};
        bool first = true;
        while (s.has_q() || s.has_pending()) {
            if (!first && sep_id.valid()) {
                auto r = do_consume(g, out, sep_id, s, depth, pretty);
                if (!r) return r;
            }
            auto r = do_consume(g, out, item_id, s, depth, pretty);
            if (!r) return r;
            first = false;
        }
        return {};
    }

    case NodeKind::Choice: {
        NodeId picked = choose_alternative_v2(g, n, s);
        if (!picked.valid()) {
            return tl::unexpected(SaveError{
                "no matching grammar alternative for value at save_v2"});
        }
        return do_consume(g, out, picked, s, depth, pretty);
    }
    }
    return {};
}

// Pretty-print wrapping around the body (parallel to legacy save_node).
tl::expected<void, SaveError>
do_consume(const Grammar& g, std::ostream& out, NodeId node_id,
           SaveState& s, int depth, bool pretty) {
    if (!node_id.valid()) return {};
    NodeId rid = g.resolve_ref(node_id);
    const Node& orig = g.node(node_id);
    const Node& n    = g.node(rid);
    const bool ref_diff = (rid.value() != node_id.value());

    int body_depth = depth;
    if (pretty) {
        if (orig.depth_in) ++body_depth;
        if (ref_diff && n.depth_in) ++body_depth;
    }

    auto emit_tab = [&](const Node& node) {
        if (!pretty || !node.indent_emit) return;
        for (int i = 0; i < body_depth; ++i) out << g.indent_step();
    };
    auto emit_post = [&](const Node& node) {
        if (!node.tail.empty())            out << node.tail;
        if (node.space_after)              out << ' ';
        if (pretty && node.newline_after)  out << '\n';
    };

    emit_tab(orig);
    if (ref_diff) emit_tab(n);

    auto r = do_consume_body(g, out, node_id, s, body_depth, pretty);
    if (!r) return r;

    if (ref_diff) emit_post(n);
    emit_post(orig);
    return {};
}

} // namespace

tl::expected<void, SaveError>
save_v2(const Grammar& g, std::ostream& out, const ValuePtr& root, bool pretty) {
    if (!root) return tl::unexpected(SaveError{"save: null root value"});
    SaveState s;
    s.push_q({root, false, ""});
    return do_consume(g, out, g.top(), s, 0, pretty);
}

} // namespace rawast
