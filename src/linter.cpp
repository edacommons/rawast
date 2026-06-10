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

// A Key reachable as a possible first-token of some grammar fragment,
// carrying its `strict` flag. Used by the prefix-collision lint pass:
// a non-strict Key whose text is a strict prefix of another Key's text
// in the same Choice will silently shadow the longer one (PEG commits
// to the first match, and byte-prefix matching has no word boundary —
// the `not` vs `notch` failure mode).
struct FirstKey {
    std::string text;
    bool        strict;

    bool operator<(const FirstKey& o) const {
        return text < o.text || (text == o.text && strict < o.strict);
    }
};

// Collect every Key that could be the first consumer of `id`, with its
// strict flag preserved. Skips Parse nodes (handled by the LL(1)
// check) and follows nullable prefixes through Sequence / Choice /
// Repeat just like first_of.
void first_keys_of(const Grammar& g, NodeId id,
                   std::set<std::size_t>& visited,
                   std::set<FirstKey>& out) {
    if (!id.valid()) return;
    NodeId resolved = g.resolve_ref(id);
    if (!resolved.valid()) return;
    if (visited.count(resolved.value())) return;
    visited.insert(resolved.value());

    const Node& n = g.node(resolved);
    switch (n.kind) {
    case NodeKind::Key: {
        auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
        if (sv) out.insert({sv->data(), n.strict});
        break;
    }
    case NodeKind::Choice:
        for (NodeId alt : n.children) {
            first_keys_of(g, alt, visited, out);
        }
        break;
    case NodeKind::Sequence: {
        std::size_t start = (n.has_separator ? 1 : 0);
        for (std::size_t i = start; i < n.children.size(); ++i) {
            std::set<std::size_t> sub_visited = visited;
            first_keys_of(g, n.children[i], sub_visited, out);
            // Stop unless this child is nullable (Value, Optional).
            NodeId rc = g.resolve_ref(n.children[i]);
            if (!rc.valid()) continue;
            const Node& cn = g.node(rc);
            bool child_nullable = (cn.kind == NodeKind::Value) || cn.is_optional;
            if (!child_nullable) break;
        }
        break;
    }
    case NodeKind::Repeat: {
        std::size_t item_idx = (n.has_separator ? 1 : 0);
        if (item_idx < n.children.size()) {
            std::set<std::size_t> sub_visited = visited;
            first_keys_of(g, n.children[item_idx], sub_visited, out);
        }
        break;
    }
    default:
        // Parse, Value, Raw, Ref — Ref already resolved above; the rest
        // don't contribute first-Keys.
        break;
    }

    visited.erase(resolved.value());
}

// `prefix` is a (non-empty) strict-prefix of `longer` if all of
// prefix's bytes match the start of longer and longer is at least one
// byte more. Same-length strings are not prefixes.
bool is_strict_prefix(const std::string& prefix, const std::string& longer) {
    if (prefix.empty()) return false;
    if (prefix.size() >= longer.size()) return false;
    return longer.compare(0, prefix.size(), prefix) == 0;
}

// LL(k) discrimination: compute the set of possible Key-token sequences
// an alternative could match, up to `max_depth` keys deep. Each element
// is a vector of Key literals at successive positions. Non-Key consumers
// (Parse, Value) are skipped — they consume input but don't contribute
// to LL(k) Key-based disambiguation. Repeat and unbounded Choice that
// can't be pinned down are treated as "indeterminate" and stop the walk
// at that depth.
//
// For two Choice alternatives to be LL(k)-disambiguated, their
// key-path sets must be disjoint (no shared prefix-sequence). The PEG
// engine naturally falls back on alt failure, so shared first-token
// alternatives with diverging second-tokens (the DEF `+ FIXED` vs
// `+ ROUTED` pattern) parse correctly without warning.
using KeyPath = std::vector<std::string>;

void key_paths_of(const Grammar& g, NodeId id,
                  std::set<std::size_t>& visited,
                  std::size_t max_depth,
                  KeyPath prefix,
                  std::set<KeyPath>& out);

// Walk children of a sequence-like node, accumulating Keys into the
// current path. Forks into multiple paths when a Choice is encountered
// (each alternative contributes its own continuation).
void key_paths_sequence(const Grammar& g,
                        const std::vector<NodeId>& children,
                        std::size_t start_idx,
                        std::set<std::size_t>& visited,
                        std::size_t max_depth,
                        KeyPath prefix,
                        std::set<KeyPath>& out) {
    if (prefix.size() >= max_depth) {
        out.insert(prefix);
        return;
    }
    if (start_idx >= children.size()) {
        out.insert(prefix);
        return;
    }
    NodeId ch_id = children[start_idx];
    NodeId resolved = g.resolve_ref(ch_id);
    if (!resolved.valid()) {
        out.insert(prefix);
        return;
    }
    const Node& ch = g.node(resolved);

    switch (ch.kind) {
    case NodeKind::Key: {
        auto sv = std::dynamic_pointer_cast<StringValue>(ch.value);
        if (sv) {
            KeyPath next = prefix;
            next.push_back(sv->data());
            key_paths_sequence(g, children, start_idx + 1, visited,
                               max_depth, std::move(next), out);
        } else {
            out.insert(prefix);
        }
        if (ch.is_optional) {
            // Optional Key — also try skipping it.
            key_paths_sequence(g, children, start_idx + 1, visited,
                               max_depth, prefix, out);
        }
        break;
    }
    case NodeKind::Parse:
    case NodeKind::Value:
        // Consumes (Parse) or emits (Value) but doesn't contribute a
        // Key to the path — walk past.
        key_paths_sequence(g, children, start_idx + 1, visited,
                           max_depth, prefix, out);
        break;
    case NodeKind::Choice: {
        // Each alt contributes its own continuation. Recurse into each
        // alt with the current prefix, then continue from start_idx+1
        // for each resulting extended prefix.
        std::set<KeyPath> alt_paths;
        for (NodeId alt : ch.children) {
            std::set<std::size_t> sub_v = visited;
            key_paths_of(g, alt, sub_v, max_depth, prefix, alt_paths);
        }
        for (const KeyPath& ap : alt_paths) {
            if (ap.size() < max_depth) {
                key_paths_sequence(g, children, start_idx + 1, visited,
                                   max_depth, ap, out);
            } else {
                out.insert(ap);
            }
        }
        break;
    }
    case NodeKind::Sequence: {
        // Nested sequence — flatten.
        std::set<KeyPath> sub_paths;
        std::size_t sub_start = (ch.has_separator ? 1 : 0);
        key_paths_sequence(g, ch.children, sub_start, visited,
                           max_depth, prefix, sub_paths);
        for (const KeyPath& sp : sub_paths) {
            if (sp.size() < max_depth) {
                key_paths_sequence(g, children, start_idx + 1, visited,
                                   max_depth, sp, out);
            } else {
                out.insert(sp);
            }
        }
        break;
    }
    case NodeKind::Repeat:
        // Indeterminate — could be 0+ occurrences. Yield current prefix
        // and continue past (the 0-occurrence case).
        key_paths_sequence(g, children, start_idx + 1, visited,
                           max_depth, prefix, out);
        break;
    case NodeKind::Ref:
    case NodeKind::Raw:
        // Ref already resolved above; Raw consumes opaque bytes and
        // doesn't contribute a Key. Walk past.
        key_paths_sequence(g, children, start_idx + 1, visited,
                           max_depth, prefix, out);
        break;
    }
}

void key_paths_of(const Grammar& g, NodeId id,
                  std::set<std::size_t>& visited,
                  std::size_t max_depth,
                  KeyPath prefix,
                  std::set<KeyPath>& out) {
    if (prefix.size() >= max_depth) {
        out.insert(prefix);
        return;
    }
    if (!id.valid()) { out.insert(prefix); return; }
    NodeId resolved = g.resolve_ref(id);
    if (!resolved.valid()) { out.insert(prefix); return; }
    if (visited.count(resolved.value())) { out.insert(prefix); return; }
    visited.insert(resolved.value());

    const Node& n = g.node(resolved);
    switch (n.kind) {
    case NodeKind::Sequence: {
        std::size_t start = (n.has_separator ? 1 : 0);
        key_paths_sequence(g, n.children, start, visited,
                           max_depth, prefix, out);
        break;
    }
    case NodeKind::Key: {
        auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
        if (sv) {
            KeyPath next = prefix;
            next.push_back(sv->data());
            out.insert(std::move(next));
        } else {
            out.insert(prefix);
        }
        break;
    }
    case NodeKind::Choice:
        for (NodeId alt : n.children) {
            std::set<std::size_t> sub_v = visited;
            key_paths_of(g, alt, sub_v, max_depth, prefix, out);
        }
        break;
    default:
        // Parse / Value / Raw / Repeat / Ref — recurse one level if
        // applicable, otherwise yield the current prefix.
        out.insert(prefix);
        break;
    }

    visited.erase(resolved.value());
}

// Two alternatives are LL(k)-distinguished if their key-path sets are
// disjoint at depth k. Returns true iff there is NO shared path of
// length 1 OR longer between the two sets — i.e. every path in `a`
// differs from every path in `b` at some position.
//
// Conservative: a shared single-Key path (e.g. both alts start with
// the same Key with no further Keys before max_depth) counts as a
// collision. The depth limit reflects how deep the lint is willing to
// look; deeper grammars beyond that point fall back to first-token
// behaviour.
bool key_paths_disjoint(const std::set<KeyPath>& a,
                        const std::set<KeyPath>& b) {
    for (const KeyPath& pa : a) {
        for (const KeyPath& pb : b) {
            std::size_t prefix_len = std::min(pa.size(), pb.size());
            if (prefix_len == 0) {
                // One alt yielded an empty path (truly indeterminate
                // up to this depth). Be conservative — treat as
                // colliding.
                return false;
            }
            bool same_prefix = true;
            for (std::size_t i = 0; i < prefix_len; ++i) {
                if (pa[i] != pb[i]) { same_prefix = false; break; }
            }
            if (same_prefix) return false;
        }
    }
    return true;
}

} // namespace

std::vector<LintIssue> lint_grammar(const Grammar& g) {
    std::vector<LintIssue> issues;

    // Reverse-lookup: NodeId -> rule name (for human-readable messages).
    // Built once and shared across all lint passes.
    std::map<std::size_t, std::string> node_to_rule;
    for (const auto& [name, id] : g.named_rules()) {
        node_to_rule.emplace(id.value(), name);
    }
    auto name_of = [&](NodeId id) -> std::string {
        auto it = node_to_rule.find(id.value());
        return it == node_to_rule.end() ? std::string("anonymous-choice")
                                        : it->second;
    };

    // LL(k) ambiguity check. For each Choice with shared-first-token
    // alternatives whose key-paths can't be disambiguated within LL(k)
    // lookahead, emit ONE issue per Choice listing all the colliding
    // tokens and alternatives together. The previous one-issue-per-token
    // form produced redundant output (same long explanation repeated per
    // token) that overwhelmed users — coalescing per Choice gives one
    // actionable line per grammar location.
    constexpr std::size_t kMaxDepth = 4;
    for (std::size_t i = 0; i < g.node_count(); ++i) {
        NodeId choice_id{i};
        const Node& n = g.node(choice_id);
        if (n.kind != NodeKind::Choice) continue;
        if (n.children.size() < 2)     continue;

        // Compute first-set per alt; collect token → [alt indices].
        std::map<std::string, std::vector<std::size_t>> token_to_alts;
        for (std::size_t a = 0; a < n.children.size(); ++a) {
            std::set<std::size_t> visited;
            FirstSet f = first_of(g, n.children[a], visited);
            for (const auto& t : f.tokens) {
                token_to_alts[t].push_back(a);
            }
        }

        // Cache key-paths per alt (computed at most once per alt).
        std::map<std::size_t, std::set<KeyPath>> alt_paths;
        auto paths_for = [&](std::size_t alt_idx) -> const std::set<KeyPath>& {
            auto it = alt_paths.find(alt_idx);
            if (it != alt_paths.end()) return it->second;
            std::set<std::size_t> v;
            std::set<KeyPath> out;
            key_paths_of(g, n.children[alt_idx], v, kMaxDepth, {}, out);
            return alt_paths.emplace(alt_idx, std::move(out)).first->second;
        };

        // For each token shared by ≥2 alts, run the LL(k) check and
        // collect the tokens / alts that remain ambiguous for THIS Choice.
        std::set<std::string> ambiguous_tokens;
        std::set<std::size_t> ambiguous_alts;
        for (const auto& [tok, alts] : token_to_alts) {
            if (alts.size() <= 1) continue;
            std::set<std::size_t> still_ambiguous;
            for (std::size_t a = 0; a < alts.size(); ++a) {
                for (std::size_t b = a + 1; b < alts.size(); ++b) {
                    if (!key_paths_disjoint(paths_for(alts[a]),
                                            paths_for(alts[b]))) {
                        still_ambiguous.insert(alts[a]);
                        still_ambiguous.insert(alts[b]);
                    }
                }
            }
            if (still_ambiguous.size() >= 2) {
                ambiguous_tokens.insert(tok);
                ambiguous_alts.insert(still_ambiguous.begin(),
                                     still_ambiguous.end());
            }
        }
        if (ambiguous_tokens.empty()) continue;

        // Emit one consolidated issue for this Choice.
        // `tokens_bare`     — comma-separated unquoted (for `issue.token`,
        //                     keeps "K:foo" addressable by downstream tooling)
        // `tokens_quoted`   — quoted, brace-listed (for the description text)
        std::string tokens_bare, tokens_quoted;
        for (const auto& t : ambiguous_tokens) {
            if (!tokens_bare.empty()) tokens_bare += ", ";
            tokens_bare += t;
            if (!tokens_quoted.empty()) tokens_quoted += ", ";
            tokens_quoted += "\"" + t + "\"";
        }
        std::string alts_str;
        for (std::size_t a : ambiguous_alts) {
            if (!alts_str.empty()) alts_str += ", ";
            alts_str += std::to_string(a);
        }

        LintIssue issue;
        issue.choice_node = choice_id;
        issue.token = tokens_bare;
        issue.alternatives.assign(ambiguous_alts.begin(),
                                  ambiguous_alts.end());
        issue.description =
            "informational [" + name_of(choice_id) + "]: " +
            std::to_string(ambiguous_alts.size()) +
            " alternatives (" + alts_str + ") share first-token(s) {" +
            tokens_quoted + "} within LL(" + std::to_string(kMaxDepth) +
            ") lookahead. Engine handles via alt-failure recovery; "
            "restructure to factor out the shared prefix if the alt-"
            "failure cost matters, or accept the pattern. "
            "See docs/AGENTS.md.";
        issues.push_back(std::move(issue));
    }

    // Prefix-collision: a non-strict Key in an earlier alternative of
    // a Choice shadows a longer Key in a later alternative. PEG
    // commits to the first match, and byte-prefix Keys have no word
    // boundary — so `"not"` will silently consume the prefix of
    // `"notch"` and the longer keyword's branch becomes unreachable.
    // Fix: either reorder so the longer Key comes first (PEG's
    // longest-match-by-source-order discipline), or write the
    // shadowing Key as `'short'` (strict, word-bounded). Either form
    // makes the dispatch correct.
    for (std::size_t i = 0; i < g.node_count(); ++i) {
        NodeId choice_id{i};
        const Node& n = g.node(choice_id);
        if (n.kind != NodeKind::Choice) continue;
        if (n.children.size() < 2)     continue;

        std::vector<std::set<FirstKey>> alt_keys(n.children.size());
        for (std::size_t a = 0; a < n.children.size(); ++a) {
            std::set<std::size_t> v;
            first_keys_of(g, n.children[a], v, alt_keys[a]);
        }

        for (std::size_t a = 0; a < alt_keys.size(); ++a) {
            for (const FirstKey& ka : alt_keys[a]) {
                if (ka.strict) continue;   // strict keys never shadow
                for (std::size_t b = a + 1; b < alt_keys.size(); ++b) {
                    for (const FirstKey& kb : alt_keys[b]) {
                        if (!is_strict_prefix(ka.text, kb.text)) continue;
                        LintIssue issue;
                        issue.choice_node  = choice_id;
                        issue.token        = "K:" + ka.text;
                        issue.alternatives = {a, b};
                        issue.description =
                            "prefix-collision [" + name_of(choice_id) + "]: "
                            "non-strict Key \"" + ka.text + "\" in alt " +
                            std::to_string(a) + " shadows Key \"" + kb.text +
                            "\" in alt " + std::to_string(b) + " — input "
                            "matching \"" + kb.text + "\" gets consumed by "
                            "alt " + std::to_string(a) + "'s prefix-match. "
                            "Fix: write `'" + ka.text + "'` (single-quote, "
                            "strict / word-bounded) in alt " +
                            std::to_string(a) + ", OR reorder so alt " +
                            std::to_string(b) + " comes before alt " +
                            std::to_string(a) + ".";
                        issues.push_back(std::move(issue));
                    }
                }
            }
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

    // `node_to_rule` and `name_of` already defined at the top of
    // lint_grammar() for use across all passes.

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
