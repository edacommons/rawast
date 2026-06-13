// Tests for the `scope` parser group — scope.paren, scope.bracket,
// scope.brace, scope.angle. The parsers each consume their opener,
// capture the balanced body, and consume their closer. The body is
// emitted as a StringValue, suitable for use with `:subparse="RULE"`.

#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_scope.hpp>

#include <sstream>

using namespace rawast;

namespace {

Grammar make_target() {
    register_std_parser_group();
    register_scope_parser_group();
    Grammar g;
    return g;
}

std::string parse_capture(Grammar& g, const std::string& input) {
    std::istringstream is{input};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE(r);
    auto sv = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(sv);
    return sv->data();
}

std::string save_back(Grammar& g, const std::string& body) {
    std::ostringstream out;
    auto r = g.save(out, make_string(body));
    REQUIRE(r);
    return out.str();
}

} // namespace

// ─── scope.paren ──────────────────────────────────────────────────────

TEST_CASE("scope.paren captures balanced body and consumes both delimiters") {
    auto g = make_target();
    const char* src = R"(
        use: std, scope
        start: <PAREN>
        PAREN: sequence { scope.paren }
    )";
    REQUIRE(load_rawast_grammar_from_string(g, src));

    CHECK(parse_capture(g, "(hello)") == "hello");
    CHECK(parse_capture(g, "()") == "");
    CHECK(parse_capture(g, "(a + b)") == "a + b");
}

TEST_CASE("scope.paren balances nested parens of all shapes") {
    auto g = make_target();
    const char* src = R"(
        use: std, scope
        start: <PAREN>
        PAREN: sequence { scope.paren }
    )";
    REQUIRE(load_rawast_grammar_from_string(g, src));

    // Same-shape nesting
    CHECK(parse_capture(g, "((a))") == "(a)");
    CHECK(parse_capture(g, "(a + (b * c))") == "a + (b * c)");

    // Cross-shape nesting — closing ) inside [] or {} doesn't terminate
    CHECK(parse_capture(g, "(a[1])") == "a[1]");
    CHECK(parse_capture(g, "(a, {b, c})") == "a, {b, c}");
    CHECK(parse_capture(g, "(a < b > c)") == "a < b > c");
}

TEST_CASE("scope.paren fails cleanly on missing opener") {
    auto g = make_target();
    const char* src = R"(
        use: std, scope
        start: <PAREN>
        PAREN: sequence { scope.paren }
    )";
    REQUIRE(load_rawast_grammar_from_string(g, src));

    std::istringstream is{"hello"};
    StreamReader sr{is};
    auto r = g.parse(sr);
    CHECK_FALSE(r);
}

TEST_CASE("scope.paren fails cleanly on unterminated body") {
    auto g = make_target();
    const char* src = R"(
        use: std, scope
        start: <PAREN>
        PAREN: sequence { scope.paren }
    )";
    REQUIRE(load_rawast_grammar_from_string(g, src));

    std::istringstream is{"(unterminated"};
    StreamReader sr{is};
    auto r = g.parse(sr);
    CHECK_FALSE(r);
}

TEST_CASE("scope.paren round-trip emits opener + body + closer") {
    auto g = make_target();
    const char* src = R"(
        use: std, scope
        start: <PAREN>
        PAREN: sequence { scope.paren }
    )";
    REQUIRE(load_rawast_grammar_from_string(g, src));

    CHECK(save_back(g, "a + b") == "(a + b)");
    CHECK(save_back(g, "") == "()");
    CHECK(save_back(g, "nested (parens)") == "(nested (parens))");
}

// ─── scope.bracket, scope.brace, scope.angle ─────────────────────────

TEST_CASE("scope.bracket captures [...] body") {
    auto g = make_target();
    const char* src = R"(
        use: std, scope
        start: <B>
        B: sequence { scope.bracket }
    )";
    REQUIRE(load_rawast_grammar_from_string(g, src));

    CHECK(parse_capture(g, "[1, 2, 3]") == "1, 2, 3");
    CHECK(parse_capture(g, "[a[0]]") == "a[0]");          // same-shape nest
    CHECK(parse_capture(g, "[(a)]") == "(a)");            // cross-shape nest
    CHECK(save_back(g, "1, 2") == "[1, 2]");
}

TEST_CASE("scope.brace captures {...} body") {
    auto g = make_target();
    const char* src = R"(
        use: std, scope
        start: <B>
        B: sequence { scope.brace }
    )";
    REQUIRE(load_rawast_grammar_from_string(g, src));

    CHECK(parse_capture(g, "{a, b}") == "a, b");
    CHECK(parse_capture(g, "{{nested}}") == "{nested}");
    CHECK(save_back(g, "x") == "{x}");
}

TEST_CASE("scope.angle captures <...> body") {
    auto g = make_target();
    const char* src = R"(
        use: std, scope
        start: <A>
        A: sequence { scope.angle }
    )";
    REQUIRE(load_rawast_grammar_from_string(g, src));

    CHECK(parse_capture(g, "<T>") == "T");
    CHECK(parse_capture(g, "<Vec<int>>") == "Vec<int>");
    CHECK(save_back(g, "T, U") == "<T, U>");
}

// Subparse integration test deferred — `#subparse=` on a Parse-kind
// child binding currently fails at the loader (the same error fires
// when loading the existing `tcl.rawast`, which uses the same pattern
// extensively). Once that loader path is fixed, this is the test:
//
//   PAREN_INT: sequence dict {
//     scope.paren:type="paren_int":value=@:#subparse="INT_RULE"
//   }
//   INT_RULE: sequence { int }
//
// Then `(42)` should parse to `{type:"paren_int", value:42}` — the
// captured body `42` re-parses through INT_RULE into an integer.
