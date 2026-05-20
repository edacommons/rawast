#pragma once

#include <rawast/grammar.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace rawast {

// One grammar-design problem flagged by the linter. The current linter
// only emits LL(1)-discipline issues: a `Choice` (without the
// `backtrack` opt-in) whose alternatives can both accept the same
// initial terminal.
struct LintIssue {
    NodeId      choice_node;            // The Choice in violation.
    std::string token;                  // The ambiguous first-token.
                                        // Encoded "K:<literal>" for a
                                        // Key, "P:<name>" for a Parse.
    std::vector<std::size_t> alternatives;  // Indices into the Choice's
                                            // children that share this
                                            // first-token.
    std::string description;            // Human-readable summary.
};

// Walk a Grammar and report all LL(1) violations on non-backtracking
// Choices. Empty result means the grammar is predictive-clean. Issues
// list the Choice NodeId, the offending first-token, the alternative
// indices that collide, and a short message.
//
// Cycles in the grammar (Ref chains, recursive rules) are detected and
// treated as not contributing further to first-sets — the linter does
// not infinite-loop.
std::vector<LintIssue> lint_grammar(const Grammar& g);

} // namespace rawast
