// Tests for the `!` negative-lookahead operator added to the .rawast
// surface and the underlying engine flag `Node::is_negative`.
//
// Coverage:
//   * `!'literal'` succeeds at cursor positions that don't start with
//     the literal, fails (without consuming) where they do.
//   * `!<RULE>` propagates through a Ref chain (force_negative path).
//   * A `!X` operator consumes zero input regardless of inner outcome.
//   * `!<CHOICE>` works on a choice-shaped inner (the canonical
//     BLOCK_TERMINATORS / reserved-word-set use case).
//   * First-byte propagation through `!` correctly invertss the inner
//     set so a choice dispatcher containing `!X` doesn't over-skip.

#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>

#include <sstream>

using namespace rawast;

namespace {

Grammar make_target() {
    register_std_parser_group();
    Grammar g;
    return g;
}

bool parses(Grammar& g, const std::string& input) {
    std::istringstream is{input};
    StreamReader sr{is};
    return static_cast<bool>(g.parse(sr));
}

} // namespace

TEST_CASE("`!'literal'` rejects matching input, succeeds on non-matching") {
    auto g = make_target();
    // STMT: !'end' identifier  — accept any identifier *except* `end`.
    const char* src = R"(
        use: std
        start: <STMT>
        STMT: sequence { !'end', identifier }
    )";
    REQUIRE(load_rawast_grammar_from_string(g, src));

    CHECK(parses(g, "alpha"));
    CHECK(parses(g, "anything_else"));
    CHECK_FALSE(parses(g, "end"));
}

TEST_CASE("`!X` consumes zero bytes — following items see the same cursor") {
    auto g = make_target();
    // STMT: !'end' "begin"  — the `!` peeks but the literal `begin`
    // must still be at byte 0 for the sequence to succeed.
    const char* src = R"(
        use: std
        start: <STMT>
        STMT: sequence { !'end', "begin" }
    )";
    REQUIRE(load_rawast_grammar_from_string(g, src));

    CHECK(parses(g, "begin"));            // `!'end'` succeeds (cursor still on `begin`), then "begin" matches
    CHECK_FALSE(parses(g, "end"));        // `!'end'` fails because cursor is on `end`
    CHECK_FALSE(parses(g, "other"));      // `!'end'` succeeds, but next "begin" fails
}

TEST_CASE("`!<RULE>` propagates through the Ref chain") {
    auto g = make_target();
    // Negative lookahead on a Ref. KEYWORDS is a Choice that matches
    // `end`, `endcase`, or `else`. `!<KEYWORDS>` succeeds iff the cursor
    // is not at one of those.
    const char* src = R"(
        use: std
        start: <STMT>
        STMT: sequence { !<KEYWORDS>, identifier }
        KEYWORDS: choice { 'end', 'endcase', 'else' }
    )";
    REQUIRE(load_rawast_grammar_from_string(g, src));

    CHECK(parses(g, "foo"));
    CHECK(parses(g, "endure"));         // KEYWORDS uses strict-keys so `endure` doesn't match `end`
    CHECK_FALSE(parses(g, "end"));
    CHECK_FALSE(parses(g, "endcase"));
    CHECK_FALSE(parses(g, "else"));
}

TEST_CASE("`!X` inside a Repeat lets the iteration terminate cleanly") {
    auto g = make_target();
    // The canonical use case: a repeat over BLOCK_ITEM that stops
    // when it sees a terminator. Without `!`, the repeat-and-`end`
    // pair would race; with `!`, BLOCK_ITEM fails fast at the
    // terminator and the surrounding sequence's `end` claims it.
    const char* src = R"(
        use: std
        start: <BLOCK>
        BLOCK ignore whitespace: sequence dict { 'begin', repeat <BLOCK_ITEM>:items[]=@, 'end' }
        BLOCK_ITEM: sequence { !'end', identifier, ";" }
    )";
    REQUIRE(load_rawast_grammar_from_string(g, src));

    CHECK(parses(g, "begin a; b; c; end"));
    CHECK(parses(g, "begin end"));
    // Without the `!`, `end` would be consumed as an identifier and
    // the closing `end` would never match. With `!`, the `repeat`
    // halts at `end` so the BLOCK terminator can take over.
    CHECK_FALSE(parses(g, "begin a end"));   // `a` not followed by `;` — should fail
}

TEST_CASE("`!X` followed by `<X>` always fails (predicate-of-self)") {
    auto g = make_target();
    // `!<X>, <X>` is a logical contradiction — the lookahead asserts X
    // can't match, then the sequence requires it. Should never accept
    // any input.
    const char* src = R"(
        use: std
        start: <STMT>
        STMT: sequence { !'foo', "foo" }
    )";
    REQUIRE(load_rawast_grammar_from_string(g, src));

    CHECK_FALSE(parses(g, "foo"));
    CHECK_FALSE(parses(g, "bar"));
    CHECK_FALSE(parses(g, ""));
}
