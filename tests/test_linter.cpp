#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/linter.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>

#include <memory>

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

TEST_CASE("Linter: Choice with two alternatives sharing initial Key is flagged") {
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("TOP", top);

    // Both alternatives start with the literal "+":
    NodeId alt1 = g.add_sequence(top);
    g.add_key(alt1, "+");
    g.add_key(alt1, "FOO");

    NodeId alt2 = g.add_sequence(top);
    g.add_key(alt2, "+");
    g.add_key(alt2, "BAR");

    g.set_top(g.new_ref("TOP"));

    auto issues = lint_grammar(g);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].choice_node == top);
    CHECK(issues[0].token == "K:+");
    CHECK(issues[0].alternatives.size() == 2);
    CHECK(issues[0].description.find("backtrack") != std::string::npos);
}

TEST_CASE("Linter: Choice with backtrack:true is silently allowed") {
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("TOP", top);
    g.set_backtrack(top);   // opt in to backtracking

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

TEST_CASE("Linter: collision across more than two alternatives reports all") {
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
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].token == "K:X");
    CHECK(issues[0].alternatives.size() == 4);
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

TEST_CASE("Linter: the bundled .rawast grammar is clean") {
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

    auto issues = lint_grammar(g);
    if (!issues.empty()) {
        FAIL("Unexpected lint issue in .rawast grammar: " << issues[0].description);
    }
    CHECK(issues.empty());
}
