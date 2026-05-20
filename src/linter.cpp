#include <rawast/linter.hpp>

#include <map>
#include <set>
#include <string>
#include <utility>

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

    return issues;
}

} // namespace rawast
