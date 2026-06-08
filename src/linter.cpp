#include <rawast/linter.hpp>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rawast {

namespace {

// "first set" for a grammar fragment: the set of initial-token
// signatures the fragment could begin with, plus a `nullable` flag
// indicating whether the fragment can also match zero input.
//
// Token signatures are encoded as strings:
//   "K:<literal>"   for a Key node (the exact text it matches)
//   "P:<parser>"    for a Parse node (the named terminal parser)
// Value-kind nodes contribute nothing — they don't consume input.
struct FirstSet {
    std::set<std::string> tokens;
    bool nullable = false;
};

FirstSet first_of(const Grammar& g, NodeId id, std::set<std::size_t>& visited);

FirstSet first_of_sequence(const Grammar& g,
                           const std::vector<NodeId>& children,
                           std::size_t start_idx,
                           std::set<std::size_t>& visited) {
    FirstSet result;
    result.nullable = true;   // a zero-child sequence matches empty
    for (std::size_t i = start_idx; i < children.size(); ++i) {
        NodeId child_id = children[i];
        NodeId resolved = g.resolve_ref(child_id);
        if (!resolved.valid()) continue;
        const Node& child = g.node(resolved);
        if (child.kind == NodeKind::Value) {
            // Doesn't consume input; skip.
            continue;
        }
        FirstSet child_first = first_of(g, child_id, visited);
        for (const auto& t : child_first.tokens) {
            result.tokens.insert(t);
        }
        if (!child_first.nullable) {
            result.nullable = false;
            break;
        }
    }
    return result;
}

FirstSet first_of(const Grammar& g, NodeId id, std::set<std::size_t>& visited) {
    FirstSet result;
    if (!id.valid()) return result;
    NodeId resolved = g.resolve_ref(id);
    if (!resolved.valid()) return result;

    if (visited.count(resolved.value())) {
        // Already on the recursion path — cycle. Return empty;
        // nullable=false (a recursive Ref alone doesn't match empty).
        return result;
    }
    visited.insert(resolved.value());

    const Node& n = g.node(resolved);
    switch (n.kind) {
    case NodeKind::Key: {
        auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
        if (sv) result.tokens.insert("K:" + sv->data());
        break;
    }
    case NodeKind::Parse: {
        auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
        if (sv) result.tokens.insert("P:" + sv->data());
        break;
    }
    case NodeKind::Value:
        // Constants don't consume input. Contributes no tokens; nullable.
        result.nullable = true;
        break;
    case NodeKind::Choice: {
        for (NodeId alt : n.children) {
            FirstSet alt_first = first_of(g, alt, visited);
            for (const auto& t : alt_first.tokens) {
                result.tokens.insert(t);
            }
            if (alt_first.nullable) result.nullable = true;
        }
        break;
    }
    case NodeKind::Sequence: {
        std::size_t start = (n.has_separator ? 1 : 0);
        result = first_of_sequence(g, n.children, start, visited);
        break;
    }
    case NodeKind::Repeat: {
        std::size_t item_idx = (n.has_separator ? 1 : 0);
        if (item_idx < n.children.size()) {
            FirstSet inner = first_of(g, n.children[item_idx], visited);
            result.tokens = std::move(inner.tokens);
        }
        result.nullable = true;   // Repeat matches zero or more
        break;
    }
    case NodeKind::Ref:
        // resolve_ref above should have unwrapped these; we shouldn't
        // get here, but be defensive.
        break;
    case NodeKind::Raw: {
        // Raw consumes any leading byte that isn't the stop literal,
        // so its first-set is "every byte except stop[0]". For LL(1)-
        // style ambiguity detection it's effectively a wildcard; mark
        // a synthetic catch-all token. Not nullable: Raw always
        // consumes at least zero bytes followed by a fail/EOF check
        // — treat as non-nullable for ambiguity purposes.
        result.tokens.insert("R:*");
        break;
    }
    }

    if (n.is_optional) result.nullable = true;

    visited.erase(resolved.value());
    return result;
}

} // namespace

std::vector<LintIssue> lint_grammar(const Grammar& g) {
    std::vector<LintIssue> issues;

    for (std::size_t i = 0; i < g.node_count(); ++i) {
        NodeId choice_id{i};
        const Node& n = g.node(choice_id);
        if (n.kind != NodeKind::Choice) continue;
        if (n.backtrack)               continue;   // opted in; skip
        if (n.children.size() < 2)     continue;   // nothing to collide

        // For each alternative, compute its first-set; collect token
        // -> [alt indices] mapping. Anything with >1 alternative is
        // a collision.
        std::map<std::string, std::vector<std::size_t>> token_to_alts;
        for (std::size_t a = 0; a < n.children.size(); ++a) {
            std::set<std::size_t> visited;
            FirstSet f = first_of(g, n.children[a], visited);
            for (const auto& t : f.tokens) {
                token_to_alts[t].push_back(a);
            }
        }

        for (const auto& [tok, alts] : token_to_alts) {
            if (alts.size() <= 1) continue;
            LintIssue issue;
            issue.choice_node    = choice_id;
            issue.token          = tok;
            issue.alternatives   = alts;
            issue.description =
                "Choice has an ambiguous first-token \"" + tok + "\" across " +
                std::to_string(alts.size()) +
                " alternatives. Without `backtrack: true` the predictive "
                "engine will commit to the first one and the others become "
                "unreachable. Either set `backtrack: true` on the Choice or "
                "restructure the grammar so the alternatives have distinct "
                "initial terminals.";
            issues.push_back(std::move(issue));
        }
    }

    // Wildcard-rule-with-nested-Choice-type-emit anti-pattern.
    //
    // Save-side Choice dispatch picks an alternative by examining
    // each alt's discriminator. When the alt is a Rule body of shape
    // `sequence dict { ... }` with NO top-level (Value-name X,
    // Value-const C) / (Value-name X, Key-with-Value-child) pair —
    // i.e. no `:type="..."`-style emit directly in the rule body —
    // the can_consume() check at can_consume_peek's Dict-container
    // fast-path returns TRUE for any input dict (no discriminator
    // can mismatch when there's no discriminator). The alt becomes
    // a wildcard.
    //
    // That's normally fine — wildcards are intentional catch-alls.
    // BUT when the rule body contains a Ref to a nested Choice whose
    // own alternatives each emit a `type=` discriminator, the rule
    // ISN'T really a catch-all: it only handles the type values its
    // nested Choice covers. The outer dispatch can't see those
    // values though, so the wildcard rule swallows ALL dict types,
    // and the inner Choice fails when the input's type is one the
    // nested Choice doesn't cover.
    //
    // Concrete failure mode (commit before c0c7029):
    //   DEF_SPECIALNET_CLAUSE: choice {
    //     <DEF_SPECIALNET_ROUTED>:clauses[]=@,           // <- alt
    //     <DEF_SPECIALNET_BODY_RECT>:clauses[]=@, ...
    //   }
    //   DEF_SPECIALNET_ROUTED: sequence dict {
    //     "+" space,
    //     <DEF_SPECIALNET_ROUTE_KW>,                     // <- nested Choice
    //     ...
    //   }
    //   DEF_SPECIALNET_ROUTE_KW: choice {
    //     <DEF_SPECIALNET_ROUTE_ROUTED>,                 // type="Routed"
    //     <DEF_SPECIALNET_ROUTE_FIXED>,                  // type="FixedRoute"
    //   }
    // No (Value-name, Value-const) pair at DEF_SPECIALNET_ROUTED's
    // top level, so it's a wildcard. A `+ RECT ...` clause
    // (type="Rect") gets dispatched there. Inside, ROUTE_KW's alts
    // don't match "Rect", and the save fails with "no matching
    // grammar alternative for value at save".
    //
    // Fix is to lift each nested Choice alternative to a sibling
    // rule of the outer Choice so each one ends up with a direct
    // type discriminator visible to the outer dispatcher.

    // Helper: does this rule body have a top-level discriminator
    // pair? Mirrors discriminator_pair_matches' SHAPE check in
    // save_stack.cpp — three legitimate shapes:
    //   (Value-name X, Value-const C)
    //   (Value-name X, Key-with-Value-child)
    //   (Value-name X, Ref-to-Choice-of-emit-keys)
    // The third shape (`<KW>:type=@` where KW is a choice of direct
    // `"FOO":@` / `"BAR":@` keys) is the LEGITIMATE multi-value
    // discriminator pattern — save's discriminator_choice_values
    // sees through it. NOT the bug case.
    auto rule_has_top_level_discriminator =
        [&g](const Node& body) -> bool {
        if (body.kind != NodeKind::Sequence) return false;
        for (std::size_t i = 0; i + 1 < body.children.size(); ++i) {
            const Node& a = g.node(g.resolve_ref(body.children[i]));
            const Node& b = g.node(g.resolve_ref(body.children[i + 1]));
            if (a.kind != NodeKind::Value || !a.is_name) continue;
            // Shape 1: Value-name + Value-const.
            if (b.kind == NodeKind::Value && !b.is_name && b.value) return true;
            // Shape 2: Value-name + Key-with-Value-child.
            if (b.kind == NodeKind::Key) {
                for (NodeId kc : b.children) {
                    const Node& kcn = g.node(g.resolve_ref(kc));
                    if (kcn.kind == NodeKind::Value && kcn.value) return true;
                }
            }
            // Shape 3: Value-name + Ref-to-Choice-of-emit-keys.
            // The Choice must have alts that are themselves direct
            // Keys with Value-child emit — the
            // discriminator_choice_values pattern.
            if (b.kind == NodeKind::Choice) {
                bool all_emit_keys = !b.children.empty();
                for (NodeId alt : b.children) {
                    const Node& altn = g.node(g.resolve_ref(alt));
                    if (altn.kind != NodeKind::Key) {
                        all_emit_keys = false; break;
                    }
                    bool emits = false;
                    for (NodeId kc : altn.children) {
                        const Node& kcn = g.node(g.resolve_ref(kc));
                        if (kcn.kind == NodeKind::Value && kcn.value) {
                            emits = true; break;
                        }
                    }
                    if (!emits) { all_emit_keys = false; break; }
                }
                if (all_emit_keys) return true;
            }
        }
        return false;
    };

    // Helper: does this node resolve to a Choice whose alternatives
    // each carry a `type=` emit (Key with Value-child somewhere
    // inside the alt's Sequence body)?
    auto choice_alts_carry_type = [&g](NodeId node_id) -> bool {
        NodeId resolved = g.resolve_ref(node_id);
        if (!resolved.valid()) return false;
        const Node& n = g.node(resolved);
        if (n.kind != NodeKind::Choice) return false;
        if (n.children.empty()) return false;
        for (NodeId alt : n.children) {
            NodeId alt_resolved = g.resolve_ref(alt);
            if (!alt_resolved.valid()) return false;
            // Walk the alt looking for a discriminator emit:
            //   * A Key node with a Value-child holding a const.
            //   * A standalone Value node (not is_name) with a value
            //     — the (Value-name, Value-const) pair shape that
            //     discriminator_const_value also accepts.
            bool has_emit = false;
            std::vector<NodeId> walk = {alt_resolved};
            for (std::size_t i = 0; i < walk.size() && !has_emit; ++i) {
                const Node& w = g.node(walk[i]);
                if (w.kind == NodeKind::Key) {
                    for (NodeId kc : w.children) {
                        const Node& kcn = g.node(g.resolve_ref(kc));
                        if (kcn.kind == NodeKind::Value && kcn.value) {
                            has_emit = true; break;
                        }
                    }
                } else if (w.kind == NodeKind::Value
                           && !w.is_name && w.value) {
                    has_emit = true;
                } else if (w.kind == NodeKind::Sequence) {
                    for (NodeId c : w.children) {
                        walk.push_back(g.resolve_ref(c));
                    }
                }
            }
            if (!has_emit) return false;
        }
        return true;
    };

    // Reverse-lookup: NodeId -> rule name (for human-readable lint
    // messages). Built once.
    std::map<std::size_t, std::string> node_to_rule;
    for (const auto& [name, id] : g.named_rules()) {
        node_to_rule.emplace(id.value(), name);
    }
    auto name_of = [&](NodeId id) -> std::string {
        auto it = node_to_rule.find(id.value());
        return it == node_to_rule.end() ? std::string("?") : it->second;
    };

    for (std::size_t i = 0; i < g.node_count(); ++i) {
        NodeId outer_id{i};
        const Node& outer = g.node(outer_id);
        if (outer.kind != NodeKind::Choice) continue;
        if (outer.children.size() < 2) continue;

        for (std::size_t a = 0; a < outer.children.size(); ++a) {
            NodeId alt_id = outer.children[a];
            NodeId alt_resolved = g.resolve_ref(alt_id);
            if (!alt_resolved.valid()) continue;
            const Node& alt = g.node(alt_resolved);

            // Find the underlying sequence-dict rule body. The alt
            // is typically a wrapper Sequence containing a Ref-to-
            // Rule plus a binding marker; alternatively it could be
            // an inline sequence-dict directly.
            NodeId rule_body_id;
            if (alt.kind == NodeKind::Sequence
                && alt.container == Container::Dict) {
                rule_body_id = alt_resolved;
            } else if (alt.kind == NodeKind::Sequence) {
                for (NodeId c : alt.children) {
                    NodeId c_resolved = g.resolve_ref(c);
                    if (!c_resolved.valid()) continue;
                    const Node& cn = g.node(c_resolved);
                    if (cn.kind == NodeKind::Sequence
                        && cn.container == Container::Dict
                        && c_resolved != alt_resolved) {
                        rule_body_id = c_resolved;
                        break;
                    }
                }
            }
            if (!rule_body_id.valid()) continue;

            const Node& body = g.node(rule_body_id);
            if (rule_has_top_level_discriminator(body)) continue;

            bool found = false;
            for (NodeId c : body.children) {
                if (choice_alts_carry_type(c)) { found = true; break; }
            }
            if (!found) continue;

            LintIssue issue;
            issue.choice_node  = outer_id;
            issue.token        = "(wildcard-with-nested-type-choice)";
            issue.alternatives = {a};
            issue.description =
                "Choice rule `" + name_of(outer_id) + "` "
                "alternative " + std::to_string(a) + " (`" +
                name_of(rule_body_id) + "`) is a dict rule with "
                "NO top-level `:type=...` discriminator, but its "
                "body contains a nested Choice whose alternatives "
                "DO emit `type=` values. Save-side dispatch can't "
                "introspect into the nested Choice, so this rule "
                "looks like a wildcard catch-all and may swallow "
                "values destined for sibling alternatives, then "
                "fail on the inner Choice with \"no matching "
                "grammar alternative for value at save\". Lift "
                "each nested-Choice alternative to a sibling rule "
                "of the outer Choice so each one ends up with a "
                "direct type discriminator visible to the outer "
                "dispatcher.";
            issues.push_back(std::move(issue));
        }
    }

    // Raw-consume sanity: every `*` node must sit inside a Sequence
    // and have a Key-with-literal as its immediate-next sibling.
    // The loader enforces the same rule and would have rejected the
    // grammar already by the time we get here — but the lint pass
    // surfaces the issue with a friendlier message during
    // `rawast lint <grammar>` instead of waiting for a load failure.
    for (std::size_t i = 0; i < g.node_count(); ++i) {
        NodeId parent_id{i};
        const Node& parent = g.node(parent_id);
        if (parent.kind != NodeKind::Sequence) continue;
        const auto& kids = parent.children;
        for (std::size_t k = 0; k < kids.size(); ++k) {
            NodeId child_id = g.resolve_ref(kids[k]);
            const Node& child = g.node(child_id);
            if (child.kind != NodeKind::Raw) continue;
            bool ok = false;
            std::string reason;
            if (k + 1 >= kids.size()) {
                reason = "nothing follows";
            } else {
                NodeId next_id = g.resolve_ref(kids[k + 1]);
                const Node& next = g.node(next_id);
                if (next.kind != NodeKind::Key) {
                    reason = "next sibling is not a literal key";
                } else {
                    auto sv = std::dynamic_pointer_cast<StringValue>(next.value);
                    if (!sv || sv->data().empty()) {
                        reason = "next-sibling key has no literal";
                    } else {
                        ok = true;
                    }
                }
            }
            if (ok) continue;
            LintIssue issue;
            issue.choice_node = child_id;
            issue.token       = "*";
            issue.description =
                "Raw consume (`*`) needs a literal-key sibling "
                "immediately after it in the same Sequence — that "
                "literal tells the engine where to stop scanning. "
                "Here: " + reason + ". Add a `\"...\"` key after the "
                "`*` (e.g. `*:body=@, \"END\" newline`).";
            issues.push_back(std::move(issue));
        }
    }

    return issues;
}

} // namespace rawast
