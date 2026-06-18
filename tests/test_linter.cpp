#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/linter.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>

#include <algorithm>
#include <memory>
#include <set>
#include <string>

using namespace rawast;

// Clean grammar: distinct first-tokens, no issues ------------------------

TEST_CASE("Linter: clean Choice with distinct keys produces no issues") {
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("TOP", top);
    g.add_key(top, "a");
    g.add_key(top, "b");
    g.add_key(top, "c");
    g.set_top(g.new_ref("TOP"));

    auto issues = lint_grammar(g);
    CHECK(issues.empty());
}

TEST_CASE("Linter: clean Choice mixing keys and parsers produces no issues") {
    Grammar g;
    g.register_parser(std::make_unique<IntParser>());
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());

    NodeId top = g.new_choice();
    g.register_rule("VALUE", top);
    g.add_parse(top, "int");
    g.add_parse(top, "string");
    g.add_key(top, "null");
    g.set_top(g.new_ref("VALUE"));

    auto issues = lint_grammar(g);
    CHECK(issues.empty());
}

// Shared-prefix collision detection ---------------------------------------

TEST_CASE("Linter: Choice with identical alternatives is flagged") {
    // Two alternatives whose key-paths are identical at every depth —
    // both just match `+ <identifier>`. The second alt is unreachable
    // because the first wins on every input the second could match.
    // Lint should flag this even though only the first Key collides
    // (the second position is a non-Key Parse, contributing no
    // discriminator).
    Grammar g;
    g.register_parser(std::make_unique<IdentifierParser>());

    NodeId top = g.new_choice();
    g.register_rule("TOP", top);

    NodeId alt1 = g.add_sequence(top);
    g.add_key(alt1, "+");
    g.add_parse(alt1, "identifier");

    NodeId alt2 = g.add_sequence(top);
    g.add_key(alt2, "+");
    g.add_parse(alt2, "identifier");

    g.set_top(g.new_ref("TOP"));

    auto issues = lint_grammar(g);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].choice_node == top);
    CHECK(issues[0].token == "K:+");
    CHECK(issues[0].alternatives.size() == 2);
    CHECK(issues[0].description.find("informational") != std::string::npos);
}

TEST_CASE("Linter: shared first-Key with diverging second-Key is NOT flagged") {
    // The PEG-natural case: alts share their FIRST Key but diverge at
    // the SECOND Key. Engine tries alt 0, fails when input doesn't
    // match alt 0's second Key, falls back to alt 1 via natural PEG
    // alt-failure recovery. LL(2) lookahead disambiguates; no warning.
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("TOP", top);

    NodeId alt1 = g.add_sequence(top);
    g.add_key(alt1, "+");
    g.add_key(alt1, "FOO");

    NodeId alt2 = g.add_sequence(top);
    g.add_key(alt2, "+");
    g.add_key(alt2, "BAR");

    g.set_top(g.new_ref("TOP"));

    auto issues = lint_grammar(g);
    CHECK(issues.empty());
}

// Prefix-collision (strict-key shadowing) -------------------------------

TEST_CASE("Linter: prefix-collision in Choice is flagged") {
    // Classic case: alternative 0's Key "not" is a non-strict prefix
    // of alternative 1's Key "notch". PEG commits to alt 0 on input
    // "notch", consuming "not" + leaving "ch" as a phantom suffix.
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("STMT", top);

    NodeId alt0 = g.add_sequence(top);
    g.add_key(alt0, "not");

    NodeId alt1 = g.add_sequence(top);
    g.add_key(alt1, "notch");

    g.set_top(g.new_ref("STMT"));

    auto issues = lint_grammar(g);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].choice_node == top);
    CHECK(issues[0].token == "K:not");
    CHECK(issues[0].alternatives.size() == 2);
    CHECK(issues[0].description.find("\"not\"") != std::string::npos);
    CHECK(issues[0].description.find("\"notch\"") != std::string::npos);
    CHECK(issues[0].description.find("'not'") != std::string::npos);
}

TEST_CASE("Linter: prefix-collision resolved when the shorter Key is strict") {
    // Same shapes as above, but alternative 0's "not" is strict
    // (word-bounded). The strict check rejects the "c" after "not"
    // in "notch", so the collision goes away.
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("STMT", top);

    NodeId alt0 = g.add_sequence(top);
    NodeId k0 = g.add_key(alt0, "not");
    g.set_strict(k0);

    NodeId alt1 = g.add_sequence(top);
    g.add_key(alt1, "notch");

    g.set_top(g.new_ref("STMT"));

    auto issues = lint_grammar(g);
    CHECK(issues.empty());
}

TEST_CASE("Linter: prefix-collision flagged regardless of backtrack flag") {
    // The lint no longer treats `backtrack: true` as a silencer — the
    // runtime always backtracks Choice frames, so the flag has no
    // runtime effect on Choice anyway. The prefix-collision warning is
    // informational design feedback; setting backtrack:true doesn't
    // suppress it.
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("STMT", top);
    g.set_backtrack(top);

    NodeId alt0 = g.add_sequence(top);
    g.add_key(alt0, "not");

    NodeId alt1 = g.add_sequence(top);
    g.add_key(alt1, "notch");

    g.set_top(g.new_ref("STMT"));

    auto issues = lint_grammar(g);
    CHECK(issues.size() == 1);
}

TEST_CASE("Linter: same-length distinct Keys are not a prefix collision") {
    // "foo" and "bar" don't collide as prefixes — they should not be
    // flagged by the prefix lint (LL(1) check also passes since the
    // first-bytes differ).
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("STMT", top);
    g.add_key(top, "foo");
    g.add_key(top, "bar");
    g.set_top(g.new_ref("STMT"));

    auto issues = lint_grammar(g);
    CHECK(issues.empty());
}

TEST_CASE("Linter: LL(k) check is independent of the backtrack flag") {
    // The `backtrack: true` attribute used to silence LL(k) warnings;
    // it no longer does (the engine always backtracks Choice frames at
    // runtime, so the attribute had no parse-side meaning on Choice).
    // The LL(k) check now reports purely on structural divergence within
    // its lookahead window. With FOO vs BAR at depth 2, the alts
    // disambiguate — no warning regardless of backtrack flag.
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("TOP", top);
    g.set_backtrack(top);

    NodeId alt1 = g.add_sequence(top);
    g.add_key(alt1, "+");
    g.add_key(alt1, "FOO");

    NodeId alt2 = g.add_sequence(top);
    g.add_key(alt2, "+");
    g.add_key(alt2, "BAR");

    g.set_top(g.new_ref("TOP"));

    auto issues = lint_grammar(g);
    CHECK(issues.empty());
}

TEST_CASE("Linter: many alternatives diverging at depth 2 — not flagged") {
    // Four alts, all sharing "X" as the first Key, each diverging at
    // a distinct second Key (A / B / C / D). LL(2) sees all four as
    // mutually distinguishable; no warning emitted.
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("TOP", top);

    for (int i = 0; i < 4; ++i) {
        NodeId alt = g.add_sequence(top);
        g.add_key(alt, "X");
        g.add_key(alt, std::string{static_cast<char>('A' + i)});
    }
    g.set_top(g.new_ref("TOP"));

    auto issues = lint_grammar(g);
    CHECK(issues.empty());
}

TEST_CASE("Linter: many alternatives, some colliding at depth 2") {
    // Four alts share "X" as the first Key. Three diverge at depth 2
    // (A / B / C), but the fourth ALSO has "A" at depth 2 — colliding
    // with alt 0. Lint should flag alts 0 and 3 (and only those), not
    // the LL(2)-disambiguated alts 1 and 2.
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("TOP", top);

    for (int i = 0; i < 4; ++i) {
        NodeId alt = g.add_sequence(top);
        g.add_key(alt, "X");
        char second = (i == 3) ? 'A' : static_cast<char>('A' + i);
        g.add_key(alt, std::string{second});
    }
    g.set_top(g.new_ref("TOP"));

    auto issues = lint_grammar(g);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].token == "K:X");
    // Alts 0 and 3 are mutually ambiguous; alts 1 and 2 are clean.
    CHECK(issues[0].alternatives.size() == 2);
    CHECK(issues[0].alternatives[0] == 0);
    CHECK(issues[0].alternatives[1] == 3);
}

TEST_CASE("Linter: nullable leading children expand the first-set") {
    // Grammar:
    //   TOP: choice { Sequence("?<A>", "x"), "x" }
    // First alt: optional A then "x"; first-set is {"K:<A's first>", "K:x"}
    // Second alt: just "x"; first-set is {"K:x"}
    // They collide on "K:x".
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("TOP", top);

    NodeId alt1 = g.add_sequence(top);
    NodeId a    = g.add_key(alt1, "a");
    g.set_optional(a);              // "?a"
    g.add_key(alt1, "x");

    NodeId alt2 = g.add_sequence(top);
    g.add_key(alt2, "x");

    g.set_top(g.new_ref("TOP"));

    auto issues = lint_grammar(g);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].token == "K:x");
}

TEST_CASE("Linter: Ref resolution lets it follow chains through rules") {
    Grammar g;

    // INNER: choice { "a", "b" }
    NodeId inner = g.new_choice();
    g.register_rule("INNER", inner);
    g.add_key(inner, "a");
    g.add_key(inner, "b");

    // TOP: choice { <INNER>, "b" }   — "b" collides with INNER's "b"
    NodeId top = g.new_choice();
    g.register_rule("TOP", top);
    g.add_ref(top, "INNER");
    g.add_key(top, "b");

    g.set_top(g.new_ref("TOP"));

    auto issues = lint_grammar(g);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].token == "K:b");
}

TEST_CASE("Linter: well-formed recursive grammar terminates without false positive") {
    // EXPR: choice { Sequence("(", <EXPR>, ")"), "a" }
    // Recursive (via the parenthesised form) but well-formed: the two
    // alternatives have distinct first-tokens "(" and "a". The linter
    // must walk the Ref<EXPR> chain without infinite-looping and must
    // not invent a collision.
    Grammar g;
    NodeId expr = g.new_choice();
    g.register_rule("EXPR", expr);

    NodeId par = g.add_sequence(expr);
    g.add_key(par, "(");
    g.add_ref(par, "EXPR");
    g.add_key(par, ")");

    g.add_key(expr, "a");
    g.set_top(g.new_ref("EXPR"));

    auto issues = lint_grammar(g);
    CHECK(issues.empty());
}

TEST_CASE("Linter: left-recursive Choice is correctly flagged") {
    // EXPR: choice { <EXPR>, "a" }
    // This is genuinely ambiguous: Ref<EXPR> resolves to a Choice whose
    // first-set is {"K:a"} (after cycle-breaking), and the second
    // alternative is also "K:a". The linter should flag this rather
    // than infinite-looping.
    Grammar g;
    NodeId expr = g.new_choice();
    g.register_rule("EXPR", expr);
    g.add_ref(expr, "EXPR");
    g.add_key(expr, "a");
    g.set_top(g.new_ref("EXPR"));

    auto issues = lint_grammar(g);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].token == "K:a");
}

// Grammar-level sanity ---------------------------------------------------

TEST_CASE("Linter: the bundled JSON grammar is clean") {
    Grammar g;
    g.register_parser(std::make_unique<IntParser>());
    g.register_parser(std::make_unique<FloatParser>());
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");
    REQUIRE(load_json_grammar_from_file(g, "grammars/json.json"));

    auto issues = lint_grammar(g);
    if (!issues.empty()) {
        // Surface the first issue for diagnostic.
        FAIL("Unexpected lint issue in JSON grammar: " << issues[0].description);
    }
    CHECK(issues.empty());
}

TEST_CASE("Linter: the bundled .rawast grammar has only intentional informational warnings") {
    // The meta-grammar uses intentional shared-prefix Choices in a few
    // places (KEY_EXPR's normal vs strict variants, REPEAT_PLUS_FORM's
    // +N vs + variants) — these trigger LL(k) informational warnings
    // but parse and round-trip correctly. The test allows informational
    // warnings on those specific Choice rules; any OTHER lint output
    // (prefix-collision, wildcard-Choice-type-emit, raw-consume misuse,
    // or LL(k) warnings on unexpected rules) still fails the test.
    Grammar g;
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g.register_parser(std::make_unique<IdentifierParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.register_parser(std::make_unique<LineCommentParser>());
    g.register_parser(std::make_unique<BlockCommentParser>());
    g.add_ignore("whitespace");
    g.add_ignore("line_comment");
    g.add_ignore("block_comment");
    REQUIRE(load_json_grammar_from_file(g, "grammars/rawast.json"));

    // Allow informational LL(k) warnings on these specific rules
    // (the grammar uses intentional shared-prefix Choices here).
    const std::set<std::string> allowed_informational = {
        "REPEAT_PLUS_FORM",
    };

    auto issues = lint_grammar(g);
    for (const auto& issue : issues) {
        // Informational warnings on allowed rules are fine.
        if (issue.description.find("informational [") != std::string::npos) {
            bool is_allowed = false;
            for (const auto& rule : allowed_informational) {
                if (issue.description.find("[" + rule + "]") != std::string::npos) {
                    is_allowed = true;
                    break;
                }
            }
            if (is_allowed) continue;
        }
        FAIL("Unexpected lint issue in .rawast grammar: " << issue.description);
    }
}

TEST_CASE("Linter: shared-leading-ref flags exponential precedence pattern") {
    // The classic PEG trap: `choice { LEVEL_BINOP, NEXT }` where the
    // BINOP form starts with a Ref to NEXT and has required items after.
    // When the operator fails, NEXT is re-parsed. With chained levels,
    // this compounds to 2^N work per call.
    //
    // The lint should flag this pattern at grammar-load time so authors
    // restructure to the linear `NEXT (OP NEXT)*` form before paying the
    // performance cost in production.
    Grammar g;
    g.register_parser(std::make_unique<IdentifierParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");

    // ADD with the buggy pattern:
    //   ADD: choice { ADD_BINOP, PRIMARY }
    //   ADD_BINOP: sequence { PRIMARY, "+", PRIMARY }
    //
    // Both alts begin by parsing PRIMARY.
    NodeId primary = g.new_sequence();
    g.add_parse(primary, "identifier");
    g.register_rule("PRIMARY", primary);

    NodeId add_binop = g.new_sequence();
    g.add_ref(add_binop, "PRIMARY");
    g.add_key(add_binop, "+");
    g.add_ref(add_binop, "PRIMARY");
    g.register_rule("ADD_BINOP", add_binop);

    NodeId add = g.new_choice();
    g.add_ref(add, "ADD_BINOP");
    g.add_ref(add, "PRIMARY");
    g.register_rule("ADD", add);
    g.set_top(g.new_ref("ADD"));

    auto issues = lint_grammar(g);
    bool found = false;
    for (const auto& issue : issues) {
        if (issue.description.find("shared-leading-ref [ADD]") != std::string::npos
            && issue.description.find("PRIMARY") != std::string::npos) {
            found = true;
            break;
        }
    }
    CHECK_MESSAGE(found, "Expected shared-leading-ref warning on ADD invoking PRIMARY");
}

TEST_CASE("Linter: shared-leading-ref does NOT flag the linear pattern") {
    // The fixed pattern `LEVEL := NEXT (OP NEXT)*` uses Sequence
    // (not Choice) at the top level, so the lint has nothing to check.
    Grammar g;
    g.register_parser(std::make_unique<IdentifierParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");

    NodeId primary = g.new_sequence();
    g.add_parse(primary, "identifier");
    g.register_rule("PRIMARY", primary);

    NodeId add_tail = g.new_sequence();
    g.add_key(add_tail, "+");
    g.add_ref(add_tail, "PRIMARY");
    g.register_rule("ADD_TAIL", add_tail);

    NodeId tail_repeat = g.new_repeat();
    g.add_ref(tail_repeat, "ADD_TAIL");

    NodeId add = g.new_sequence();
    g.set_container(add, Container::Dict);
    g.add_ref(add, "PRIMARY");
    g.node(add).children.push_back(tail_repeat);
    g.register_rule("ADD", add);
    g.set_top(g.new_ref("ADD"));

    auto issues = lint_grammar(g);
    for (const auto& issue : issues) {
        CHECK_MESSAGE(issue.description.find("shared-leading-ref") == std::string::npos,
                      "Linear pattern should not be flagged: " << issue.description);
    }
}

// ─── Byte-scan stop sanity (Raw + Scope, single-stop + multi-stop) ─────

namespace {
// Did at least one issue's description contain `fragment`?
bool any_description_contains(const std::vector<LintIssue>& issues,
                              const std::string& fragment) {
    return std::any_of(issues.begin(), issues.end(),
        [&](const LintIssue& i) {
            return i.description.find(fragment) != std::string::npos;
        });
}
} // namespace

TEST_CASE("Linter: Raw without next sibling is flagged") {
    Grammar g;
    NodeId seq = g.new_sequence();
    g.register_rule("BAD", seq);
    g.new_raw();  // detached, but the rule body sequence has Raw as
                  // its only child with no Key sibling after.
    NodeId raw = g.new_raw();
    g.node(seq).children.push_back(raw);
    g.set_top(g.new_ref("BAD"));

    auto issues = lint_grammar(g);
    CHECK(any_description_contains(issues, "Raw consume (`*`)"));
    CHECK(any_description_contains(issues, "nothing follows"));
}

TEST_CASE("Linter: Raw with non-Key next sibling is flagged") {
    Grammar g;
    g.register_parser(std::make_unique<IntParser>());
    NodeId seq = g.new_sequence();
    g.register_rule("BAD", seq);
    NodeId raw = g.new_raw();
    g.node(seq).children.push_back(raw);
    g.add_parse(seq, "int");  // Parse, not Key — invalid stop boundary.
    g.set_top(g.new_ref("BAD"));

    auto issues = lint_grammar(g);
    CHECK(any_description_contains(issues, "Raw consume (`*`)"));
    CHECK(any_description_contains(issues, "not a literal key"));
}

TEST_CASE("Linter: Raw inside repeat without separator + post-repeat Key is flagged") {
    Grammar g;
    NodeId raw = g.new_raw();
    NodeId rep = g.new_repeat();
    g.node(rep).children.push_back(raw);

    NodeId seq = g.new_sequence();
    g.register_rule("BAD", seq);
    g.add_key(seq, "(");
    g.node(seq).children.push_back(rep);
    // Intentionally no closing Key after the repeat — multi-stop pass
    // should flag the missing post-repeat sibling.
    g.set_top(g.new_ref("BAD"));

    auto issues = lint_grammar(g);
    CHECK(any_description_contains(issues, "inside a repeat"));
    CHECK(any_description_contains(issues, "nothing follows the repeat"));
}

TEST_CASE("Linter: Raw inside repeat with non-Key separator is flagged") {
    Grammar g;
    g.register_parser(std::make_unique<IntParser>());
    NodeId raw = g.new_raw();
    NodeId rep = g.new_repeat();
    g.node(rep).children.push_back(raw);
    // Non-Key separator — invalid as a multi-stop boundary.
    NodeId sep_parse = g.new_parse("int");
    g.set_separator(rep, sep_parse);

    NodeId seq = g.new_sequence();
    g.register_rule("BAD", seq);
    g.add_key(seq, "(");
    g.node(seq).children.push_back(rep);
    g.add_key(seq, ")");  // post-repeat sibling IS a Key — good.
    g.set_top(g.new_ref("BAD"));

    auto issues = lint_grammar(g);
    CHECK(any_description_contains(issues, "inside a repeat"));
    CHECK(any_description_contains(issues, "repeat separator"));
}

TEST_CASE("Linter: Raw inside repeat with both bounds Key is clean") {
    Grammar g;
    NodeId raw = g.new_raw();
    NodeId rep = g.new_repeat();
    g.node(rep).children.push_back(raw);
    NodeId sep_key = g.new_key(",");
    g.set_separator(rep, sep_key);

    NodeId seq = g.new_sequence();
    g.register_rule("OK", seq);
    g.add_key(seq, "(");
    g.node(seq).children.push_back(rep);
    g.add_key(seq, ")");
    g.set_top(g.new_ref("OK"));

    auto issues = lint_grammar(g);
    // Either no issues, or none mentioning the Raw — Choice ambiguity
    // checks may still emit other unrelated issues for synthetic
    // grammars but the Raw-specific ones shouldn't fire.
    CHECK_FALSE(any_description_contains(issues, "Raw consume"));
}

// Scope-shape sanity goes through the loader since there's no public
// new_scope() — loaded grammars exercise the lint paths via integration.
TEST_CASE("Linter: scope grammars from the loader pass clean") {
    register_std_parser_group();
    Grammar g;
    auto r = load_rawast_grammar_from_string(g, R"GRAM(
        use: std
        start: <PROG>
        PROG: sequence array {
          "(",
          repeat scope { std.string } separator ",",
          ")"
        }
    )GRAM");
    REQUIRE_MESSAGE(r, "load failed: " << (r ? "" : r.error()));

    auto issues = lint_grammar(g);
    CHECK_FALSE(any_description_contains(issues, "Scope"));
}
