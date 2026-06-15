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
#include <functional>
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
    // Per-field iteration cursor for list-append (`:name[]=@`) bindings.
    // Each pull from a `name[]`-suffixed pending advances the cursor;
    // the field is marked `consumed` only when the cursor reaches the
    // list's end.
    std::map<std::string, std::size_t> list_progress;
};

// Strip a trailing `[]` from a binding name. Returns the bare name
// and sets `was_list` if the suffix was present.
inline std::string strip_list_suffix(const std::string& name, bool& was_list) {
    if (name.size() >= 2
        && name.compare(name.size() - 2, 2, "[]") == 0) {
        was_list = true;
        return name.substr(0, name.size() - 2);
    }
    was_list = false;
    return name;
}

class SaveState {
public:
    // --- queue ---
    void push_q(QueueEntry e) { queue_.push_back(std::move(e)); }
    bool has_q() const { return q_idx_ < queue_.size(); }
    const QueueEntry& peek_q() const { return queue_[q_idx_]; }
    QueueEntry next_q() { return std::move(queue_[q_idx_++]); }

    // --- dict scope ---
    void push_dict(const DictValue& d) { dict_stack_.push_back({&d, {}, {}}); }
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
            bool was_list = false;
            const std::string name = strip_list_suffix(pending_, was_list);
            clear_pending();
            auto it = scope.dict->data().find(name);
            if (it != scope.dict->data().end()) {
                // List-append (`name[]`) form: return the next
                // element of dict[name] as an array, advance the
                // per-field cursor, mark consumed when exhausted.
                if (was_list) {
                    auto arr = as_array(it->second);
                    if (!arr) {
                        return tl::unexpected(SaveError{
                            "list-append binding (`" + name + "[]`) "
                            "expected ArrayValue but field is "
                            "non-array"});
                    }
                    std::size_t idx = scope.list_progress[name];
                    if (idx >= arr->data().size()) {
                        if (is_optional) {
                            return QueueEntry{nullptr, false, name};
                        }
                        return tl::unexpected(SaveError{
                            "list-append exhausted for '" + name + "'"});
                    }
                    scope.list_progress[name] = idx + 1;
                    if (idx + 1 >= arr->data().size()) {
                        scope.consumed.insert(name);
                    }
                    return QueueEntry{arr->data()[idx], false, name};
                }
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
        // Inline Choice whose alternatives have V-name markers inside
        // them (e.g. EVENT_CONTROL's `choice { sequence { "*":wildcard=true },
        // ... }`). Without this, the dict would flatten to open-schema
        // mode (key/value queue pairs) — and the Choice dispatch would
        // see no top_dict, failing every alt that needs to look up a
        // discriminator field. Recurse one level into inline Choice
        // alts.
        if (cn.kind == NodeKind::Choice) {
            for (NodeId alt : cn.children) {
                const Node& alt_orig = g.node(alt);
                if (alt_orig.kind == NodeKind::Ref) return true;
                const Node& alt_n = g.node(g.resolve_ref(alt));
                if (alt_n.kind == NodeKind::Sequence
                    && has_name_markers(g, alt_n)) {
                    return true;
                }
            }
        }
        // Repeat node whose item has a V-name marker (the
        // `repeat <X>:f[]=@` pattern desugars to V-name + expr INSIDE
        // the Repeat's item position). Same fixed-schema-vs-open-schema
        // distinction: TASK_ARG_LIST's `repeat <EXPR>:args[]=@` would
        // otherwise be treated as open-schema and flatten the dict's
        // entries into the queue — sending the FIELD NAME (a String)
        // to the Repeat's item dispatch instead of the array elements.
        if (cn.kind == NodeKind::Repeat) {
            std::size_t item_idx = cn.has_separator ? 1 : 0;
            if (item_idx < cn.children.size()) {
                const Node& item = g.node(g.resolve_ref(cn.children[item_idx]));
                if (!item.children.empty()) {
                    const Node& first =
                        g.node(g.resolve_ref(item.children[0]));
                    if (first.kind == NodeKind::Value && first.is_name) {
                        return true;
                    }
                }
            }
        }
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
        auto sv = as_string(cn.value);
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

// Forward decl: comparator used by discriminator_pair_matches before
// values_equal_v2's definition appears later in the file.
bool values_equal_v2(const ValuePtr& a, const Value* b);

ValuePtr discriminator_const_value(const Grammar& g, const Node& b) {
    if (b.kind == NodeKind::Value && !b.is_name) return b.value;
    if (b.kind == NodeKind::Key) {
        // Explicit Value-child (the `'X':@` emit form): use that.
        for (NodeId kc : b.children) {
            const Node& kcn = g.node(g.resolve_ref(kc));
            if (kcn.kind == NodeKind::Value && kcn.value) return kcn.value;
        }
        // Otherwise: the Key's own literal IS the discriminator value
        // when paired with a V-name marker (the `"X":name=@` pattern,
        // where the loaded shape is V-name + bare Key). The pair-walk
        // in has_explicit_discriminator / can_consume only ever calls
        // this on `b` AFTER seeing a V-name `a`, so returning the
        // literal here is safe — structural Keys (`","`, `";"`) that
        // aren't paired with a V-name simply don't reach this code
        // path.
        if (b.value) return b.value;
    }
    return nullptr;
}

// Multi-valued discriminator: handles the additional case where `b`
// is a Ref-to-Choice whose alternatives are all Key-with-Value-child
// emit-true keys (e.g. `<VIA_KW>` for `<VIA_KW>:type=@` where VIA_KW
// is `choice { "VIA":@, "Via":@ }`). Returns every literal the
// referenced Choice could emit. Empty list means "this isn't the
// multi-value-discriminator pattern; use discriminator_const_value
// for the single-value case."
std::vector<ValuePtr>
discriminator_choice_values(const Grammar& g, const Node& b) {
    std::vector<ValuePtr> values;
    if (b.kind != NodeKind::Choice) return values;
    for (NodeId alt : b.children) {
        const Node& a = g.node(g.resolve_ref(alt));
        if (a.kind != NodeKind::Key) return {};  // not the pattern
        ValuePtr v = discriminator_const_value(g, a);
        if (!v) return {};
        values.push_back(std::move(v));
    }
    return values;
}

// Does the (Value-name `name`, expr `b`) pair pin down a dict field
// to a specific value (or set of values) that the input dict satisfies?
// Returns:
//   * std::nullopt — pair is not a discriminator (e.g. b is a Parse).
//   * false — discriminator exists but the dict's field doesn't match.
//   * true — discriminator exists and dict matches.
// Sees through a Ref so `<VIA_KW>` (a Choice-of-emit-keys) discriminates
// just like an inline `"VIA":@` would.
// Walk a Sequence node's (V-name, X) pairs and collect every discriminator
// they introduce. Each entry is (field_name, accepted_values) — the field
// must equal one of the values. Used by the (V-name, Repeat<Item>) handling
// below to forward the Repeat's item-level discriminators to the outer
// alt's selection.
struct ChildDisc {
    std::string field;
    std::vector<ValuePtr> values;  // dict[field] must equal one of these
};

std::vector<ChildDisc>
collect_child_discriminators(const Grammar& g, const Node& seq, int depth = 0) {
    std::vector<ChildDisc> out;
    if (seq.kind != NodeKind::Sequence) return out;
    // Recursion depth guard against grammars where a Choice's alts
    // reach back into this Sequence through a Ref chain (rare but
    // possible; surfaced as a stack overflow without the cap).
    if (depth > 4) return out;
    for (std::size_t i = 0; i + 1 < seq.children.size(); ++i) {
        const Node& a = g.node(g.resolve_ref(seq.children[i]));
        const Node& b = g.node(g.resolve_ref(seq.children[i + 1]));
        if (a.kind != NodeKind::Value || !a.is_name) continue;
        auto fname = as_string(a.value);
        if (!fname) continue;
        if (ValuePtr cv = discriminator_const_value(g, b)) {
            out.push_back({fname->data(), {std::move(cv)}});
            continue;
        }
        auto multi = discriminator_choice_values(g, b);
        if (!multi.empty()) {
            out.push_back({fname->data(), std::move(multi)});
        }
    }
    // Inline Choice children whose alts each carry a single
    // discriminator on the same field are themselves discriminators:
    // the dict's field must be in the union of alt values. Without
    // this, a rule like
    //
    //   TAIL: sequence dict {
    //       choice { '+':op="+", '-':op="-" },
    //       <NEXT>:rhs=@
    //   }
    //
    // exposes no discriminator to its enclosing Repeat — so a chain
    // rule whose tail items use TAIL looks like a wildcard and
    // accepts any dict whose `tail` field happens to be non-empty.
    for (NodeId child_id : seq.children) {
        const Node& c = g.node(g.resolve_ref(child_id));
        if (c.kind != NodeKind::Choice) continue;
        if (c.children.empty()) continue;
        std::string common_field;
        std::vector<ValuePtr> values;
        bool ok = true;
        for (NodeId alt : c.children) {
            const Node& a = g.node(g.resolve_ref(alt));
            auto alt_discs = collect_child_discriminators(g, a, depth + 1);
            if (alt_discs.size() != 1) { ok = false; break; }
            if (alt_discs[0].values.size() != 1) { ok = false; break; }
            if (common_field.empty()) {
                common_field = alt_discs[0].field;
            } else if (alt_discs[0].field != common_field) {
                ok = false; break;
            }
            values.push_back(alt_discs[0].values[0]);
        }
        if (ok && !values.empty()) {
            out.push_back({std::move(common_field), std::move(values)});
        }
    }
    return out;
}

std::optional<bool>
discriminator_pair_matches(const Grammar& g, const std::string& name,
                            const Node& b_resolved, const DictValue& dict) {
    // Single-value form: Value-const or Key-with-Value-child.
    if (ValuePtr cv = discriminator_const_value(g, b_resolved)) {
        auto it = dict.data().find(name);
        if (it == dict.data().end()) return false;
        return values_equal_v2(it->second, cv.get());
    }
    // Multi-value form: Ref-to-Choice-of-emit-keys.
    auto values = discriminator_choice_values(g, b_resolved);
    if (!values.empty()) {
        auto it = dict.data().find(name);
        if (it == dict.data().end()) return false;
        for (const auto& v : values) {
            if (values_equal_v2(it->second, v.get())) return true;
        }
        return false;
    }
    return std::nullopt;
}

// Recursive check: could this grammar node dispatch the given dict?
// Used by the Sequence-Dict can_consume branch to validate inline
// Choice children (and Choices nested inside them) without
// committing to a particular dispatch path.
//
// Mirrors the prototype's `_check_element` — symmetry with parse:
// any shape parse can accept, save can dispatch through the same
// recursion. Without this, an inline Choice with non-trivial alts
// (nested Choices, Refs, Sequences with their own discriminators)
// reads as a catch-all to the dispatcher and bad dicts commit then
// fail at emission with "no matching grammar alternative for value
// at save."
//
// Recursion rules:
//
//   * Choice — at least one alt must be dispatchable.
//   * Ref    — resolve and recurse into the body.
//   * Sequence — recurse into Choice / Ref children; check every
//     (V-name + expr) pair as a discriminator (false → reject this
//     subtree). Other children don't constrain at this level.
//   * Other  — permissive; let the actual emission path decide.
bool dispatchable_for_dict(const Grammar& g, NodeId node_id,
                            const DictValue& dict,
                            std::unordered_set<std::uint32_t>& visited) {
    NodeId resolved = g.resolve_ref(node_id);
    // Cycle guard. TCL has BRACKET_WORD → ... → SCRIPT → COMMAND →
    // ... → BRACKET_WORD (command substitution inside a script).
    // Without the visited set the recursion stack-overflows on any
    // real Tcl input. When a node is already on the stack, assume
    // it's dispatchable — the surrounding context will reject
    // mismatches at the actual emission site.
    if (!visited.insert(resolved.value()).second) return true;
    bool result;
    {
        const Node& n = g.node(resolved);
        switch (n.kind) {
            case NodeKind::Choice: {
                if (n.children.empty()) { result = true; break; }
                result = false;
                for (NodeId alt : n.children) {
                    if (dispatchable_for_dict(g, alt, dict, visited)) {
                        result = true;
                        break;
                    }
                }
                break;
            }
            case NodeKind::Sequence: {
                // Scope-aware walk: a V-name marker "claims" the
                // next sibling — that sibling consumes a field's
                // value (or iterates a field's array), so its own
                // discriminators apply to the FIELD'S value, not the
                // outer dict. Without distinguishing this case from
                // a bare Ref / Choice, we'd recurse into `<X>` with
                // the outer dict for `<X>:foo=@` and reject because
                // X's discriminator doesn't match outer-level fields.
                result = true;
                bool prev_was_vname = false;
                for (std::size_t i = 0; i < n.children.size(); ++i) {
                    const Node& c = g.node(g.resolve_ref(n.children[i]));
                    if (c.kind == NodeKind::Value && c.is_name) {
                        // V-name marker. Try the discriminator-pair
                        // form against the next sibling (Value-const,
                        // Key-with-Value-child, Ref-to-Choice-of-keys
                        // — discriminator_pair_matches checks those).
                        // For non-discriminator siblings (Refs,
                        // Choices, Parse), no constraint is enforced
                        // here; the sibling will be consumed by the
                        // V-name binding at emission time and its
                        // own discriminators apply to the field's
                        // value, not the outer dict.
                        if (i + 1 < n.children.size()) {
                            auto fname = as_string(c.value);
                            if (fname) {
                                const Node& nb = g.node(
                                    g.resolve_ref(n.children[i + 1]));
                                auto m = discriminator_pair_matches(
                                    g, fname->data(), nb, dict);
                                if (m.has_value() && !*m) {
                                    result = false;
                                    break;
                                }
                            }
                        }
                        prev_was_vname = true;
                        continue;
                    }
                    if (prev_was_vname) {
                        // Consumed by the preceding V-name binding;
                        // skip discriminator-recursion on it.
                        prev_was_vname = false;
                        continue;
                    }
                    if (c.kind == NodeKind::Choice
                        || c.kind == NodeKind::Ref) {
                        if (!dispatchable_for_dict(g, n.children[i],
                                                    dict, visited)) {
                            result = false;
                            break;
                        }
                    }
                }
                break;
            }
            default:
                result = true;
                break;
        }
    }
    visited.erase(resolved.value());
    return result;
}

// Detect the `repeat <X>:field[]=@` pattern at a Sequence child position
// and check whether the dict satisfies it. The loader represents this as
// a Repeat node whose single child is a wrapper Sequence containing:
//   [V-name "field[]", <X>]
// — so the V-name is INSIDE the Repeat, not adjacent to it at the outer
// Sequence level. This function pulls the inner V-name out, then checks
// whether <X>'s own discriminators match the dict's array entries.
//
// Used by the outer Sequence-Dict can_consume walk for the precedence-
// ladder chain pattern:
//   `<NEXT>:lhs=@, repeat+ <X_TAIL>:tail[]=@`
// where X_TAIL is `sequence dict { "||":op="||", <NEXT>:rhs=@ }`.
// Without this, the save-side Choice dispatcher commits to the first
// chain alt whose top-level surface shape matches and silently drops
// or mis-emits tail entries whose op string belongs to a different
// precedence level.
//
// Returns the same tri-state as discriminator_pair_matches:
//   * nullopt — node isn't a Repeat-with-discriminating-item.
//   * false — pattern recognised but the dict's array doesn't satisfy it.
//   * true — pattern recognised and matches.
std::optional<bool>
repeat_field_matches(const Grammar& g, const Node& repeat_node,
                      const DictValue& dict) {
    if (repeat_node.kind != NodeKind::Repeat) return std::nullopt;
    std::size_t item_idx = repeat_node.has_separator ? 1 : 0;
    if (item_idx >= repeat_node.children.size()) return std::nullopt;
    const Node& wrapper = g.node(
        g.resolve_ref(repeat_node.children[item_idx]));
    if (wrapper.kind != NodeKind::Sequence) return std::nullopt;
    if (wrapper.children.size() < 2) return std::nullopt;
    const Node& vn = g.node(g.resolve_ref(wrapper.children[0]));
    if (vn.kind != NodeKind::Value || !vn.is_name) return std::nullopt;
    auto vn_name = as_string(vn.value);
    if (!vn_name) return std::nullopt;
    bool is_list = false;
    std::string field = strip_list_suffix(vn_name->data(), is_list);
    if (!is_list) return std::nullopt;
    const Node& item = g.node(g.resolve_ref(wrapper.children[1]));
    auto discs = collect_child_discriminators(g, item);
    // n-ary chain pattern: the separator can carry the rule's
    // discriminator as a const-binding (`separator '||':op="||"`).
    // Collect those too; if neither item nor separator yields any,
    // there's no discriminator and the function is inapplicable.
    std::vector<ChildDisc> sep_discs;
    if (repeat_node.has_separator && !repeat_node.children.empty()) {
        const Node& sep = g.node(g.resolve_ref(repeat_node.children[0]));
        sep_discs = collect_child_discriminators(g, sep);
    }
    if (discs.empty() && sep_discs.empty()) return std::nullopt;
    auto it = dict.data().find(field);
    if (it == dict.data().end()) {
        return repeat_node.min > 0 ? std::optional<bool>{false} : std::nullopt;
    }
    auto arr = std::dynamic_pointer_cast<ArrayValue>(it->second);
    if (!arr) return std::nullopt;
    if (arr->data().empty()) {
        return repeat_node.min > 0 ? std::optional<bool>{false} : std::nullopt;
    }
    for (const auto& elt : arr->data()) {
        auto elt_dict = std::dynamic_pointer_cast<DictValue>(elt);
        if (!elt_dict) return std::nullopt;
        for (const auto& d : discs) {
            auto eit = elt_dict->data().find(d.field);
            if (eit == elt_dict->data().end()) return false;
            bool ok = false;
            for (const auto& v : d.values) {
                if (values_equal_v2(eit->second, v.get())) { ok = true; break; }
            }
            if (!ok) return false;
        }
    }
    // n-ary chain pattern: verify the rule-level discriminator the
    // separator carries (e.g. `separator '||':op="||"` means
    // dict[op] must equal "||" for this rule to apply). Without
    // this check, two CHAIN rules differing only in operator —
    // OR_CHAIN with '||', AND_CHAIN with '&&' — both look like
    // catch-alls and the dispatcher picks whichever comes first.
    for (const auto& d : sep_discs) {
        auto dit = dict.data().find(d.field);
        if (dit == dict.data().end()) return false;
        bool ok = false;
        for (const auto& v : d.values) {
            if (values_equal_v2(dit->second, v.get())) {
                ok = true;
                break;
            }
        }
        if (!ok) return false;
    }
    return true;
}

// Does this Choice alternative have any explicit discriminator?
// Explicit discriminator forms recognised:
//   * `(Value-name X, Value-const C)` adjacent — dict[X]==C must hold.
//   * `(Value-name X, Key K with Value-child C)` adjacent — same.
//   * Bare Repeat child whose wrapper carries a V-name + an item with
//     its own discriminator (the `repeat <X>:field[]=@` precedence-
//     ladder chain pattern).
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
            // Ref-to-Choice-of-emit-keys also counts (LEF `<VIA_KW>:
            // type=@` pattern).
            if (!discriminator_choice_values(g, b).empty()) return true;
        }
        // Bare Repeat children carrying a V-name + discriminating
        // item, OR a separator with a const-binding discriminator.
        // The second case covers the n-ary chain pattern:
        //   repeat+2 <NEXT>:args[]=@ separator '||':op="||"
        // where the rule-distinguishing constant lives on the
        // separator, not on the item. Without this branch the rule
        // looks like a wildcard catch-all and the save dispatcher
        // routes every {args:...}-shaped dict here regardless of op.
        for (NodeId child_id : n.children) {
            const Node& cn = g.node(g.resolve_ref(child_id));
            if (cn.kind != NodeKind::Repeat) continue;
            std::size_t item_idx = cn.has_separator ? 1 : 0;
            if (item_idx < cn.children.size()) {
                const Node& wrapper = g.node(
                    g.resolve_ref(cn.children[item_idx]));
                if (wrapper.kind == NodeKind::Sequence
                    && wrapper.children.size() >= 2) {
                    const Node& item = g.node(
                        g.resolve_ref(wrapper.children[1]));
                    if (!collect_child_discriminators(g, item).empty()) {
                        return true;
                    }
                }
            }
            if (cn.has_separator && !cn.children.empty()) {
                const Node& sep = g.node(g.resolve_ref(cn.children[0]));
                if (!collect_child_discriminators(g, sep).empty()) {
                    return true;
                }
            }
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
        // Strip list-append suffix so `items[]` looks up the `items`
        // array and peeks the next un-consumed element. Without this,
        // a `repeat <X>:items[]=@` Choice dispatch can never find its
        // discriminator (dict has `items`, not `items[]`). Falls back
        // to scalar field lookup for non-list bindings.
        bool is_list = false;
        std::string field = strip_list_suffix(s.pending_name(), is_list);
        auto it = s.top_dict()->dict->data().find(field);
        if (it != s.top_dict()->dict->data().end()) {
            if (is_list) {
                auto arr = as_array(it->second);
                if (arr && !arr->data().empty()) {
                    auto pit = s.top_dict()->list_progress.find(field);
                    std::size_t idx = pit == s.top_dict()->list_progress.end()
                                        ? 0 : pit->second;
                    if (idx < arr->data().size()) {
                        peek_value = arr->data()[idx];
                        peek_key   = field;
                    }
                }
            } else {
                peek_value = it->second;
                peek_key   = field;
            }
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
            auto tok = as_string(n.value);
            auto vs  = as_string(peek_value);
            if (tok && vs && tok->data() == vs->data()) return true;
        }
        // Match against the originating dict key (open-schema flatten).
        if (n.value && !peek_key.empty()) {
            auto tok = as_string(n.value);
            if (tok && tok->data() == peek_key) return true;
        }
        // Trivial-emit fallback: with no peek to discriminate against,
        // a Key whose only Value-child holds its own text (the `"X":@`
        // emit pattern, used in e.g. VIA_DEFAULT_LITERAL's
        // `"DEFAULT":@ / "Default":@`) is dispatchable as a no-op
        // canonical form — picks the first alt the surrounding Choice
        // tries. Lets `<VIA_DEFAULT_LITERAL>:is_default=true` style
        // bindings emit the keyword without needing a per-alternative
        // discriminator value in the input dict.
        if (!peek_value && peek_key.empty()) {
            for (NodeId c : n.children) {
                const Node& cn = g.node(g.resolve_ref(c));
                if (cn.kind == NodeKind::Value && cn.value) return true;
            }
        }
        return false;
    }

    case NodeKind::Parse: {
        if (!peek_value) return false;
        auto name = as_string(n.value);
        if (!name) return false;
        const std::string& p = name->data();
        ValueType vt = peek_value->type();

        // Standard parsers: strict value-type match. Order is
        // important — `int`/`uint` is checked before the generic
        // custom-parser fallback so `{"type":"string"}` doesn't claim
        // to handle an IntValue.
        if (p == "string") return vt == ValueType::String;
        if (p == "identifier") {
            // The dispatch check covers two parser groups that both
            // register `identifier`: `std.identifier` (strict
            // `[A-Za-z_][A-Za-z0-9_]*`) and `lefdef.identifier`
            // (permissive — accepts dots, digits, brackets, slashes,
            // hyphens; the LEF/DEF spec for things like `5.8`, `0`,
            // `M1[7:0]`, `inst/pin`, `Via1Array-0`).
            //
            // Probe the registered parser with the *actual* value:
            // identifier dispatches iff the parser would consume the
            // entire string on re-parse. That guarantees round-trip
            // (the unparsed text re-parses back as an identifier) and
            // correctly rejects strings with spaces, semicolons, or
            // quotes the identifier parser can't handle.
            if (vt != ValueType::String) return false;
            const auto& s =
                std::static_pointer_cast<StringValue>(peek_value)->data();
            if (s.empty()) return false;
            if (Parser* pp = g.parser(p)) {
                std::istringstream is{s};
                StreamReader sr{is};
                auto r = pp->parse(sr);
                if (r && sr.peek() == std::nullopt) {
                    return true;
                }
                return false;
            }
            // No parser registered (shouldn't happen): fall back to
            // the strict shape check.
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
            // Walk (Value-name, expr) discriminator pairs — all
            // matching pairs must agree with the input dict. `expr`
            // may be a Value-const, a Key-with-Value-child, or a
            // Ref-to-Choice-of-emit-keys (LEF's `<VIA_KW>:type=@`
            // pattern). Pairs that don't form a discriminator are
            // skipped. If no discriminator pair exists at all, the
            // alt is a dict-shaped catch-all (matches any dict).
            const auto& dict = *dynamic_cast<const DictValue*>(peek_value.get());
            bool found_disc_match = false;
            for (std::size_t i = 0; i + 1 < n.children.size(); ++i) {
                const Node& a = g.node(g.resolve_ref(n.children[i]));
                // Check the ORIGINAL child node (before resolve_ref) for
                // is_optional — the flag is on the Ref, not the resolved
                // rule body. An absent dict field for an optional pair
                // shouldn't reject the alt.
                const Node& b_orig = g.node(n.children[i + 1]);
                const Node& b = g.node(g.resolve_ref(n.children[i + 1]));
                if (a.kind != NodeKind::Value || !a.is_name) continue;
                auto name_sv = as_string(a.value);
                if (!name_sv) continue;
                auto m = discriminator_pair_matches(g, name_sv->data(), b, dict);
                if (!m.has_value()) continue;  // not a discriminator
                if (!*m) {
                    // Optional pair + absent field: not a mismatch, skip.
                    if (b_orig.is_optional
                        && dict.data().find(name_sv->data()) == dict.data().end()) {
                        continue;
                    }
                    return false;
                }
                found_disc_match = true;
            }
            // Repeat-with-discriminating-item pattern: the loader wraps
            // `repeat <X>:field[]=@` as a Repeat whose item is a wrapper
            // sequence carrying the V-name inside, NOT adjacent to the
            // Repeat at the outer Sequence level. So the (V-name, sibling)
            // walk above can't see it; check each Repeat child directly
            // for the precedence-ladder discriminator pattern.
            for (NodeId child_id : n.children) {
                const Node& orig = g.node(child_id);
                const Node& cn = g.node(g.resolve_ref(child_id));
                auto m = repeat_field_matches(g, cn, dict);
                if (!m.has_value()) continue;
                if (!*m) {
                    if (orig.is_optional) continue;
                    return false;
                }
                found_disc_match = true;
            }
            // Inline-Choice walk via the recursive dispatchability
            // check. Symmetric with parse: a Choice in the rule body
            // dispatches if at least one alt could fire on the
            // current dict — checked recursively so nested Choices,
            // Refs, and Sequences with their own discriminators all
            // participate. Without this, the rule reads as a catch-
            // all and the dispatcher commits to it for any dict
            // whose field-presence walk passes — then errors at
            // emission with "no matching grammar alternative for
            // value at save."
            for (NodeId child_id : n.children) {
                const Node& orig = g.node(child_id);
                // Only fire on INLINE Choice children — Refs that
                // resolve to a Choice body are scope-changers (the
                // V-name preceding the Ref takes care of dispatch),
                // not in-body discriminators.
                if (orig.kind != NodeKind::Choice) continue;
                std::unordered_set<std::uint32_t> visited;
                if (!dispatchable_for_dict(g, child_id, dict, visited)) {
                    if (orig.is_optional) continue;
                    return false;
                }
                found_disc_match = true;
            }
            if (found_disc_match) return true;
            // No discriminator matched. Fall back to a field-presence
            // check: every REQUIRED (non-optional) Value-name marker
            // whose consumer would pull a value from this dict scope
            // must have its field present. Without this, a rule like
            // `sequence dict { <LOR>:lhs=@ }` would be treated as a
            // catch-all (matches any dict) even when the dict lacks the
            // `lhs` field — leading to "missing required field 'lhs'"
            // failures downstream.
            for (std::size_t i = 0; i + 1 < n.children.size(); ++i) {
                const Node& a = g.node(g.resolve_ref(n.children[i]));
                if (a.kind != NodeKind::Value || !a.is_name) continue;
                auto name_sv = as_string(a.value);
                if (!name_sv) continue;
                bool is_list = false;
                std::string field = strip_list_suffix(name_sv->data(), is_list);
                const Node& b_orig = g.node(n.children[i + 1]);
                if (b_orig.is_optional) continue;
                // Constant (V-const) sibling doesn't pull from dict —
                // it just produces a value. Skip.
                const Node& b = g.node(g.resolve_ref(n.children[i + 1]));
                if (b.kind == NodeKind::Value && !b.is_name) continue;
                // Required field must exist (list-append OK with empty
                // array since `repeat min=0` is the common case).
                if (dict.data().find(field) == dict.data().end()) {
                    return false;
                }
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
                auto name_sv = as_string(a.value);
                if (!name_sv) continue;
                ValuePtr cv = discriminator_const_value(g, b);
                if (!cv) continue;
                auto it = scope->dict->data().find(name_sv->data());
                if (it == scope->dict->data().end()) return false;
                if (!values_equal_v2(it->second, cv.get())) return false;
                // Reject if the field is already consumed (used by
                // a `repeat <Choice>` over a catcher body so that
                // `:is_fixed_mask=true`-style discriminators don't
                // re-fire every iteration).
                if (scope->consumed.count(name_sv->data())) return false;
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
                    if (auto sv = as_string(cn.value)) {
                        sim_pending = sv->data();
                        have_sim_pending = true;
                    }
                    continue;
                }
                if (cn.kind == NodeKind::Value) continue;
                if (cn.kind == NodeKind::Key) continue;   // structural

                // Real consumer.
                if (!have_sim_pending) {
                    // No V-name marker preceded this consumer. Three
                    // shapes recognized below; everything else means
                    // "this child doesn't help discriminate", so we
                    // continue without marking walked_consumer.
                    //
                    // 1. Wrapper Sequence (`Sequence/Container::None`
                    //    whose children are V-name + expr). Loader
                    //    wraps Choice / Optional targets this way so
                    //    the binding sticks with the expr.
                    //
                    // 2. Repeat node whose item child has a leading
                    //    V-name marker (the list-append binding lives
                    //    INSIDE the Repeat's item, not as a sibling).
                    //
                    // 3. Inline Choice node — the dispatch happens
                    //    deeper; don't try to validate here.
                    if (cn.kind == NodeKind::Sequence
                        && cn.container == Container::None) {
                        if (can_consume_peek(g, c, peek_value, peek_key, s)) {
                            walked_consumer = true;
                        }
                    } else if (cn.kind == NodeKind::Repeat
                               && !cn.children.empty()) {
                        std::size_t item_idx = cn.has_separator ? 1 : 0;
                        if (item_idx < cn.children.size()) {
                            const Node& item =
                                g.node(g.resolve_ref(cn.children[item_idx]));
                            if (!item.children.empty()) {
                                const Node& first =
                                    g.node(g.resolve_ref(item.children[0]));
                                if (first.kind == NodeKind::Value
                                    && first.is_name) {
                                    auto fname = as_string(first.value);
                                    if (fname) {
                                        bool is_list = false;
                                        std::string field =
                                            strip_list_suffix(fname->data(),
                                                              is_list);
                                        auto it =
                                            scope->dict->data().find(field);
                                        if (it != scope->dict->data().end()
                                            && !scope->consumed.count(field)) {
                                            // For list bindings, also
                                            // verify the array has
                                            // unconsumed elements. A
                                            // fully-consumed list
                                            // shouldn't keep the alt
                                            // dispatchable.
                                            if (is_list) {
                                                if (auto arr =
                                                    as_array(it->second)) {
                                                    auto pit =
                                                        scope->list_progress.find(field);
                                                    std::size_t idx =
                                                        pit == scope->list_progress.end()
                                                          ? 0 : pit->second;
                                                    if (idx < arr->data().size()) {
                                                        walked_consumer = true;
                                                    }
                                                }
                                            } else {
                                                walked_consumer = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    continue;
                }
                if (have_sim_pending) {
                    bool is_list = false;
                    std::string field = strip_list_suffix(sim_pending, is_list);
                    auto it = scope->dict->data().find(field);
                    const bool consumer_optional =
                        node_is_optional_chain(g, c);
                    if (it == scope->dict->data().end()) {
                        // Optional consumer + absent field: skip
                        // the marker (the optional clause opts
                        // out), don't reject the whole alt.
                        if (consumer_optional) {
                            have_sim_pending = false;
                            continue;
                        }
                        return false;
                    }
                    if (scope->consumed.count(field)) {
                        if (consumer_optional) {
                            have_sim_pending = false;
                            continue;
                        }
                        return false;
                    }
                    // For list-append `[]` bindings, peek at the
                    // currently-pending list element (the one a
                    // matching dispatch would pull next) instead of
                    // the whole array. Lets `repeat <Choice>` over a
                    // `:name[]=@` alternative know it can still
                    // dispatch one more element.
                    ValuePtr peek = it->second;
                    if (is_list) {
                        auto arr = as_array(it->second);
                        if (!arr || arr->data().empty()) return false;
                        auto pit = scope->list_progress.find(field);
                        std::size_t idx = pit == scope->list_progress.end()
                                            ? 0 : pit->second;
                        if (idx >= arr->data().size()) return false;
                        peek = arr->data()[idx];
                    }
                    if (!can_consume_peek(g, c, peek, field, s)) {
                        if (consumer_optional) {
                            have_sim_pending = false;
                            continue;
                        }
                        return false;
                    }
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
                    auto tok = as_string(cn.value);
                    if (tok) {
                        auto vs = as_string(peek_value);
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

    case NodeKind::Choice: {
        if (!peek_value) return false;
        for (NodeId alt : n.children) {
            if (can_consume_peek(g, alt, peek_value, peek_key, s)) return true;
        }
        return false;
    }
    case NodeKind::Repeat:
        // Repeat at top of an alt — assume it would dispatch.
        return peek_value != nullptr;

    case NodeKind::Value:
        // Used as a child during walks, not as a dispatch root.
        return true;

    case NodeKind::Raw:
        // Raw at the top of an alt — consumes whatever value is queued.
        // Without #subparse, the parse-side stored a StringValue and
        // the save side writes it verbatim, so dispatch requires a
        // string. With #subparse, the parse-side stored a structured
        // sub-tree (Dict/Array) that save will re-serialize through
        // the subparse rule (see NodeKind::Raw branch in
        // do_consume_body), so any non-null value is dispatchable —
        // the dispatch check must mirror the save path's tolerance
        // for non-string values when subparse_start is valid.
        if (!peek_value) return false;
        if (n.subparse_start.valid()) return true;
        return peek_value->type() == ValueType::String;

    case NodeKind::Scope:
        // Scope at the top of an alt — same shape as Raw. Parse-side
        // produced a StringValue holding the body bytes; with
        // #subparse it's a structured sub-tree.
        if (!peek_value) return false;
        if (n.subparse_start.valid()) return true;
        return peek_value->type() == ValueType::String;

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
    for (NodeId alt : choice_node.children) {
        if (!has_explicit_discriminator(g, alt)) continue;
        if (can_consume(g, alt, s)) return alt;
    }
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
        return tl::unexpected(SaveError{"internal: unresolved Ref at save"});

    case NodeKind::Value: {
        if (n.is_name) {
            // Name marker — set pending; next consumer looks it up.
            auto sv = as_string(n.value);
            if (sv) s.set_pending(sv->data());
        } else {
            // Constant — closes a (Value-name, Value-const) discriminator
            // pair. Look up dict[pending] in the current scope:
            //   * matches const  → pair satisfied, clear pending AND
            //     mark the field as consumed in the dict scope. The
            //     consumed mark is what lets a `repeat <Choice>` over
            //     a catcher body (e.g. `repeat <MACRO_PROPERTY>`) tell
            //     "FIXEDMASK has already emitted" from "FIXEDMASK is
            //     still to come" on the next iteration.
            //   * mismatch       → leave pending; a following optional
            //     block is expected to emit the override (ITEM's
            //     `type=bare` default + BIND_TAIL_OPT override pattern).
            //   * no dict scope  → no save-time effect (Value-const in
            //     container=None alternatives like REF dispatch).
            if (s.has_pending() && s.top_dict()) {
                const auto& d = s.top_dict()->dict->data();
                auto it = d.find(s.pending_name());
                if (it != d.end() && values_equal_v2(it->second, n.value.get())) {
                    s.top_dict()->consumed.insert(s.pending_name());
                    s.clear_pending();
                }
            }
        }
        return {};
    }

    case NodeKind::Key: {
        auto sv = as_string(n.value);
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
        auto name = as_string(n.value);
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

        // Subparse inverse: when a Parse terminal carries a
        // subparse_start rule, the AST stores the structured
        // sub-tree (from the subparse re-entry on the parse side),
        // not the original captured string. To round-trip, save
        // the sub-tree through the subparse rule first to recover
        // the textual form, then pass that text through the
        // Parse terminal's own unparse for any final formatting.
        if (n.subparse_start.valid()) {
            std::ostringstream sub_out;
            SaveState sub_s;
            sub_s.push_q({v, false, ""});
            auto sub_r = do_consume(g, sub_out, n.subparse_start,
                                    sub_s, 0, pretty);
            if (!sub_r) return tl::unexpected(sub_r.error());
            v = make_string(sub_out.str());
        }

        auto u = p->unparse(*v);
        if (!u) return tl::unexpected(u.error());
        out << *u;
        return {};
    }

    case NodeKind::Raw: {
        // Raw consume's save side is symmetric to its parse side: the
        // captured string is pulled from the dict scope and written
        // verbatim. No parser dispatch, no terminator emission — the
        // following Key sibling is responsible for writing its literal.
        const bool is_optional = n.is_optional;
        auto r = s.pull_value(is_optional);
        if (!r) return tl::unexpected(r.error());
        ValuePtr v = r->value ? r->value : null_value();

        // Subparse inverse: if the Raw carries a subparse_start,
        // the stored value is the structured sub-tree (from parse-
        // side re-entry). Serialize it back through the subparse
        // rule first to recover the text, then write that text.
        if (n.subparse_start.valid()) {
            std::ostringstream sub_out;
            SaveState sub_s;
            sub_s.push_q({v, false, ""});
            auto sub_r = do_consume(g, sub_out, n.subparse_start,
                                    sub_s, 0, pretty);
            if (!sub_r) return tl::unexpected(sub_r.error());
            out << sub_out.str();
            return {};
        }

        auto sv = as_string(v);
        if (!sv) {
            return tl::unexpected(SaveError{
                "Raw save expects a StringValue payload"});
        }
        out << sv->data();
        return {};
    }

    case NodeKind::Scope: {
        // Scope save: emit OPEN literal + captured body + CLOSE literal.
        // The body StringValue was produced verbatim on the parse side
        // (bytes between OPEN and CLOSE, exclusive); INNERs were
        // atomic spans whose text round-trips through the body. With
        // #subparse, the stored value is the structured sub-tree —
        // re-serialize through the subparse rule to recover the body
        // text first.
        if (n.children.size() < 2) {
            return tl::unexpected(SaveError{
                "scope save: node needs OPEN and CLOSE children"});
        }
        auto literal_of = [&](NodeId id) -> std::string {
            const Node& cn = g.node(g.resolve_ref(id));
            if (cn.kind != NodeKind::Key) return {};
            auto sv = as_string(cn.value);
            return sv ? sv->data() : std::string{};
        };
        const std::string open_str  = literal_of(n.children.front());
        const std::string close_str = literal_of(n.children.back());
        if (open_str.empty() || close_str.empty()) {
            return tl::unexpected(SaveError{
                "scope save: OPEN/CLOSE must be non-empty Key literals"});
        }

        const bool is_optional = n.is_optional;
        auto r = s.pull_value(is_optional);
        if (!r) return tl::unexpected(r.error());
        ValuePtr v = r->value ? r->value : null_value();

        std::string body_text;
        if (n.subparse_start.valid()) {
            std::ostringstream sub_out;
            SaveState sub_s;
            sub_s.push_q({v, false, ""});
            auto sub_r = do_consume(g, sub_out, n.subparse_start,
                                    sub_s, 0, pretty);
            if (!sub_r) return tl::unexpected(sub_r.error());
            body_text = sub_out.str();
        } else {
            auto sv = as_string(v);
            if (!sv) {
                return tl::unexpected(SaveError{
                    "scope save expects a StringValue payload"});
            }
            body_text = sv->data();
        }

        out << open_str << body_text << close_str;
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
                    // Per-child optional skip applies inside an
                    // optional Sequence too — e.g. `?<STRANS_GROUP>`
                    // resolves to a Sequence whose children include
                    // `?gdsii.mag:mag=@` and `?gdsii.angle:angle=@`.
                    // Without this check, the wrapped Sequence emits
                    // every child unconditionally and produces an
                    // empty `angle: []` record on save (which
                    // re-parses back as a present-but-empty field,
                    // breaking GDSII binary round-trip on real cells).
                    if (would_skip_optional(g, c, s)) {
                        s.clear_pending();
                        continue;
                    }
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
        //
        // Optional-chain check FIRST: a `?<RULE>` where RULE's body
        // is a container Sequence Dict (with its own discriminator)
        // must NOT pull from the outer scope if its discriminator
        // can't match — otherwise we pull a value that isn't ours
        // and crash with "container Sequence with null value." Same
        // shape as the container=None optional check above.
        if ((n.is_optional || node_is_optional_chain(g, node_id))
            && !can_consume(g, rid, s)) {
            s.clear_pending();
            return {};
        }
        auto pulled = s.pull_value(/*is_optional=*/false);
        if (!pulled) return tl::unexpected(pulled.error());
        ValuePtr v = pulled->value;
        if (!v) return tl::unexpected(SaveError{
            "container Sequence with null value"});

        if (n.container == Container::Array) {
            auto arr = as_array(v);
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
        auto dict = as_dict(v);
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

        // Continue while either:
        //   (a) the queue / pending name has data the item can pull
        //       (open-schema / array iteration mode), OR
        //   (b) the top dict scope has an unconsumed field that the
        //       inner item can dispatch on (fixed-schema catcher mode
        //       — e.g. `repeat <SITE_PROPERTY>` inside SITE_BLOCK).
        // Pre-fix, only (a) was checked; fixed-schema Repeats exited
        // immediately without emitting their body sub-statements.
        auto fixed_schema_dispatchable = [&]() {
            const DictScope* scope = s.top_dict();
            if (!scope) return false;
            for (const auto& [k, v] : scope->dict->data()) {
                if (scope->consumed.count(k)) continue;
                // For list-typed fields whose target binding is a
                // list-append (`:k[]=@`) alternative, peek the next
                // unconsumed element so the inner Choice's
                // dispatch sees one entry at a time instead of the
                // whole array. Falls back to the raw value for
                // scalar fields.
                ValuePtr peek = v;
                if (auto arr = as_array(v)) {
                    auto pit = scope->list_progress.find(k);
                    std::size_t idx = pit == scope->list_progress.end()
                                        ? 0 : pit->second;
                    if (idx < arr->data().size()) {
                        peek = arr->data()[idx];
                    } else {
                        continue;  // array exhausted
                    }
                }
                if (can_consume_peek(g, item_id, peek, k, s)) return true;
            }
            return false;
        };

        bool first = true;
        while (s.has_q() || s.has_pending() || fixed_schema_dispatchable()) {
            if (!first && sep_id.valid()) {
                auto r = do_consume(g, out, sep_id, s, depth, pretty);
                if (!r) return r;
            }
            // Snapshot full state so we can detect a no-progress
            // iteration and bail rather than spin forever if the
            // inner item failed to advance anything (queue, pending,
            // or dict-scope consumed).
            auto pre_snap = s.snapshot();
            // Track both `consumed` set growth AND list_progress
            // cursor advancement — a list-append `:k[]=@` binding
            // doesn't mark its field as fully consumed until the
            // last element is pulled, so we'd otherwise mis-detect
            // mid-list iterations as no-progress.
            const std::size_t pre_consumed =
                s.top_dict() ? s.top_dict()->consumed.size() : 0;
            std::size_t pre_list_total = 0;
            if (const DictScope* sc = s.top_dict()) {
                for (const auto& [_, idx] : sc->list_progress) {
                    pre_list_total += idx;
                }
            }
            auto r = do_consume(g, out, item_id, s, depth, pretty);
            if (!r) return r;
            auto post_snap = s.snapshot();
            const std::size_t post_consumed =
                s.top_dict() ? s.top_dict()->consumed.size() : 0;
            std::size_t post_list_total = 0;
            if (const DictScope* sc = s.top_dict()) {
                for (const auto& [_, idx] : sc->list_progress) {
                    post_list_total += idx;
                }
            }
            const bool queue_advanced = post_snap.q_idx != pre_snap.q_idx
                                         || post_snap.queue_size != pre_snap.queue_size;
            const bool pending_changed = post_snap.have_pending != pre_snap.have_pending
                                          || post_snap.pending != pre_snap.pending;
            const bool consumed_grew = post_consumed > pre_consumed;
            const bool list_advanced = post_list_total > pre_list_total;
            if (!queue_advanced && !pending_changed && !consumed_grew
                && !list_advanced) {
                break;
            }
            first = false;
        }
        return {};
    }

    case NodeKind::Choice: {
        NodeId picked = choose_alternative_v2(g, n, s);
        if (!picked.valid()) {
            // Use-site optional pattern `?<RULE>` where RULE's body is
            // a Choice: silently skip when no alt can dispatch (matches
            // the existing optional handling on Sequence). Lets
            // `?<PROPDEF_DEFAULT>`-style optional Choice references
            // round-trip cleanly when the field is absent.
            if (n.is_optional || node_is_optional_chain(g, node_id)) {
                s.clear_pending();
                return {};
            }
            // Include the rule name (if known) for diagnostics.
            std::string detail;
            for (const auto& [rule_name, rule_id] : g.named_rules()) {
                if (rule_id == rid) {
                    detail = " (rule " + rule_name + ")";
                    break;
                }
            }
            return tl::unexpected(SaveError{
                "no matching grammar alternative for value at save"
                + detail});
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

// Inverse of `compact_opchain` (defined in grammar.cpp). When the
// save engine is about to dispatch a value through an opchain-marked
// rule, the input AST is in `{op, args[]}` form; the rule's grammar
// expects always-wrap `{lhs, tail:[{op,rhs}, ...]}`. Expand
// recursively before save.
//
// Top-level wrap: a non-chain atom (e.g. `{type:"num", value:1}`) is
// wrapped as `{lhs: atom, tail: []}` so the rule's `<NEXT>:lhs=@`
// binding can find the `lhs` field. The caller decides when to wrap
// (only at the top of an opchain rule's dispatch, not on every
// recursion — args inside compacted chains are leaves at the NEXT
// precedence level and shouldn't be wrapped here).
// Build a map from `op` string → tail-rule-group id. Two ops share a
// group id iff they appear in the same grammar rule's `op`
// discriminator set — that is, the same tail rule accepts them
// both. Used by `expand_opchain` to decide whether `args[0]` and the
// outer chain op are part of the same chain (absorb) or separate
// sub-expressions at different precedence levels (don't absorb).
//
// Example for sv_pp_expr:
//   OR_TAIL    {"||"}              → group 0
//   AND_TAIL   {"&&"}              → group 1
//   EQ_TAIL    {"==", "!="}        → group 2
//   COMPARE    {"<", ">", "<=", ">="} → group 3
//   ADD_TAIL   {"+", "-"}          → group 4
//   MUL_TAIL   {"*", "/", "%"}     → group 5
//   NOT_EXPR   {"!"}               → group 6  (unary, args.size==1; never absorbed)
//
// `1 + 2 - 3` compacted: outer "-" → 4, inner "+" → 4. Same → absorb
// into a single ADD chain `{lhs:1, tail:[{+,2}, {-,3}]}`.
//
// `A == B || C == D` compacted: outer "||" → 0, inner "==" → 2.
// Different → don't absorb; the inner EQ chain stays as a complete
// `{lhs, tail}` sub-tree at OR's lhs position.
std::unordered_map<std::string, int>
build_op_compat_map(const Grammar& g) {
    std::unordered_map<std::string, int> out;
    int next_id = 0;
    for (const auto& [_, rule_id] : g.named_rules()) {
        NodeId rid = g.resolve_ref(rule_id);
        if (rid.value() >= g.node_count()) continue;
        const Node& n = g.node(rid);
        if (n.kind != NodeKind::Sequence) continue;
        auto discs = collect_child_discriminators(g, n);
        // Find the discriminator on the `op` field, if any.
        for (const auto& d : discs) {
            if (d.field != "op") continue;
            int id = next_id++;
            for (const auto& v : d.values) {
                if (auto s = as_string(v)) {
                    // First-seen mapping wins. Within one grammar each
                    // op normally appears in exactly one tail rule.
                    out.emplace(s->data(), id);
                }
            }
            break;
        }
    }
    return out;
}

// Returns true iff both ops route to the same tail rule (same group
// id in the compat map). Unknown ops never absorb (conservative).
bool ops_in_same_tail_rule(
    const std::unordered_map<std::string, int>& op_compat,
    const std::string& a, const std::string& b) {
    auto ait = op_compat.find(a);
    auto bit = op_compat.find(b);
    if (ait == op_compat.end() || bit == op_compat.end()) return false;
    return ait->second == bit->second;
}

ValuePtr expand_opchain(
    const ValuePtr& v,
    const std::unordered_map<std::string, int>& op_compat) {
    if (!v) return v;

    if (auto arr = as_array(v)) {
        auto out = std::make_shared<ArrayValue>();
        for (const auto& e : arr->data()) {
            out->data().push_back(expand_opchain(e, op_compat));
        }
        return out;
    }

    auto d = as_dict(v);
    if (!d) return v;

    // Detect compacted shape: `{op:OP, args:[a, b, ...]}` with at
    // least two args. Unary `{op:"!", args:[x]}` (NOT_EXPR etc.)
    // stays as-is; it's not a chain.
    auto op_it = d->data().find("op");
    auto args_it = d->data().find("args");
    auto args_arr = (args_it != d->data().end())
        ? as_array(args_it->second) : nullptr;
    bool is_chain = (op_it != d->data().end())
                    && args_arr
                    && args_arr->data().size() >= 2;

    if (!is_chain) {
        // Not compacted at this level — recurse on field values so
        // nested compacted nodes get expanded too.
        auto out = std::make_shared<DictValue>();
        for (const auto& [k, val] : d->data()) {
            out->data().emplace(k, expand_opchain(val, op_compat));
        }
        return out;
    }

    auto op_sv = as_string(op_it->second);
    if (!op_sv) return v;
    const std::string& op = op_sv->data();

    // Op-aware absorb: if args[0] is itself a chain whose op routes
    // to the SAME tail rule as the outer op, the two are part of one
    // chain (e.g. `1+2-3` → outer "-" and inner "+" are both
    // ADD_TAIL). Absorb args[0]'s lhs as our lhs and prepend its
    // tail. Otherwise — different precedence level — keep args[0]
    // expanded as a complete sub-tree at the outer lhs position.
    ValuePtr first = expand_opchain(args_arr->data()[0], op_compat);
    ValuePtr lhs;
    std::vector<ValuePtr> tail;
    bool absorbed = false;
    if (auto fd = as_dict(first)) {
        auto fl_it = fd->data().find("lhs");
        auto ft_it = fd->data().find("tail");
        if (fl_it != fd->data().end() && ft_it != fd->data().end()) {
            // first is a fully-expanded chain wrapper. Check if its
            // ORIGINAL op (before expand collapsed lhs/tail) belongs
            // to the same tail rule as outer. We recover it from the
            // tail entries' op — they all have the chain's op.
            auto ft_arr = as_array(ft_it->second);
            if (ft_arr && !ft_arr->data().empty()) {
                auto te0 = as_dict(ft_arr->data()[0]);
                if (te0) {
                    auto teo = te0->data().find("op");
                    if (teo != te0->data().end()) {
                        if (auto teos = as_string(teo->second)) {
                            if (ops_in_same_tail_rule(
                                op_compat, op, teos->data())) {
                                lhs = fl_it->second;
                                for (const auto& t : ft_arr->data()) {
                                    tail.push_back(t);
                                }
                                absorbed = true;
                            }
                        }
                    }
                }
            }
        }
    }
    if (!absorbed) {
        lhs = first;
    }

    // Each remaining arg becomes a tail entry `{op:OP, rhs:arg}`.
    for (std::size_t i = 1; i < args_arr->data().size(); ++i) {
        auto te = std::make_shared<DictValue>();
        te->data().emplace("op", make_string(op));
        te->data().emplace("rhs", expand_opchain(args_arr->data()[i], op_compat));
        tail.push_back(te);
    }

    auto out = std::make_shared<DictValue>();
    out->data().emplace("lhs", lhs);
    auto tail_arr_v = std::make_shared<ArrayValue>();
    for (const auto& t : tail) tail_arr_v->data().push_back(t);
    out->data().emplace("tail", tail_arr_v);
    // Carry any other wrapper-level fields (not op/args).
    for (const auto& [k, val] : d->data()) {
        if (k == "op" || k == "args") continue;
        out->data().emplace(k, expand_opchain(val, op_compat));
    }
    return out;
}

// Ensure top-level value is wrapped as `{lhs:..., tail:[]}` if it
// isn't already a chain shape. Used by save-side opchain dispatch
// so the start rule's `<NEXT>:lhs=@` binding always finds the
// `lhs` field, even for atoms (`1` → `{lhs:{type:num,value:1},
// tail:[]}`).
ValuePtr wrap_atom_as_chain(const ValuePtr& v) {
    if (!v) return v;
    if (auto d = as_dict(v); d && d->data().count("lhs")) return v;
    auto out = std::make_shared<DictValue>();
    out->data().emplace("lhs", v);
    out->data().emplace("tail", std::make_shared<ArrayValue>());
    return out;
}

} // namespace

// Definition of Grammar::save lives here (rather than in
// grammar.cpp) so the entire save engine — the helpers in this
// file's anonymous namespace plus the entry point — stays
// together. grammar.cpp has a one-line locator comment near
// the rest of the Grammar member definitions.
tl::expected<void, SaveError>
Grammar::save(std::ostream& out, ValuePtr value, bool pretty,
              NodeId start) const {
    if (!value) return tl::unexpected(SaveError{"save: null root value"});
    // Precompute the resolved-Ref cache; resolve_ref becomes O(1)
    // for the duration of this save call. Idempotent and amortised
    // across subsequent calls.
    ensure_refs_resolved_();
    if (!start.valid()) start = top();
    // `#opchain` pre-process: when the start rule chain carries the
    // flag, the input AST is in compacted `{op, args[]}` form but
    // the grammar expects always-wrap `{lhs, tail:[...]}`. Expand
    // recursively, then wrap atoms so the top-level rule's
    // `<NEXT>:lhs=@` binding always has a `lhs` field to dispatch.
    if (has_opchain_in_chain(start)) {
        auto op_compat = build_op_compat_map(*this);
        value = expand_opchain(value, op_compat);
        // Wrap atoms only when the start chain resolves to a
        // Sequence-Dict (plain always-wrap pattern, like
        // test_opchain's flat ADD grammar). For the
        // `choice { CHAIN, NEXT }` ladder pattern used by sv_pp_expr,
        // atoms naturally fall through the catch-all NEXT and the
        // wrap would break dispatch (the dict would have
        // `lhs`+`tail:[]` instead of the leaf's `type` field, so
        // PRIMARY's type-discriminated leaves wouldn't match).
        NodeId resolved_start = resolve_ref(start);
        if (resolved_start.value() < node_count()
            && node(resolved_start).kind == NodeKind::Sequence) {
            value = wrap_atom_as_chain(value);
        }
    }
    SaveState s;
    s.push_q({value, false, ""});
    return do_consume(*this, out, start, s, 0, pretty);
}

} // namespace rawast
