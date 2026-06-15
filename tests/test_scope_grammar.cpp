// `scope { OPEN, INNER..., CLOSE }` — string-aware bracket scanner.
//
// Walks the input from OPEN to the matching CLOSE, treating each INNER
// as an atomic span (so embedded strings, comments, nested scopes
// can't trigger a false CLOSE). Captured body bytes (between OPEN end
// and CLOSE start, exclusive) emit as a StringValue.

#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/value.hpp>

#include <sstream>

using namespace rawast;

namespace {

Grammar load(const char* src) {
    register_std_parser_group();
    Grammar g;
    auto r = load_rawast_grammar_from_string(g, src);
    REQUIRE_MESSAGE(r, "load failed: " << (r ? "" : r.error()));
    return g;
}

ValuePtr parse_input(Grammar& g, const std::string& input) {
    std::istringstream is{input};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE_MESSAGE(r, "parse failed for '" << input << "': "
                       << (r ? "" : r.error().message));
    return *r;
}

std::string save_value(Grammar& g, ValuePtr v) {
    std::ostringstream out;
    auto r = g.save(out, std::move(v));
    REQUIRE_MESSAGE(r, "save failed: " << (r ? "" : r.error().message));
    return out.str();
}

} // namespace

// ─── Naive scope (no INNERs): just OPEN + raw bytes + CLOSE ──────────────

TEST_CASE("scope: bare paren scope captures body bytes") {
    auto g = load(R"GRAM(
        use: std
        start: <PAREN>
        PAREN: scope { "(", ")" }
    )GRAM");
    auto v = parse_input(g, "(hello world)");
    auto sv = as_string(v);
    REQUIRE(sv);
    CHECK(sv->data() == "hello world");
    CHECK(save_value(g, v) == "(hello world)");
}

TEST_CASE("scope: empty body round-trips") {
    auto g = load(R"GRAM(
        use: std
        start: <PAREN>
        PAREN: scope { "(", ")" }
    )GRAM");
    auto v = parse_input(g, "()");
    auto sv = as_string(v);
    REQUIRE(sv);
    CHECK(sv->data() == "");
    CHECK(save_value(g, v) == "()");
}

// ─── String-aware: embedded ')' inside a string doesn't end the scope ────

TEST_CASE("scope: embedded string with ')' inside is captured atomically") {
    auto g = load(R"GRAM(
        use: std
        start: <PAREN>
        PAREN: scope { "(", std.string, ")" }
    )GRAM");
    // Input: ( "wait)here" trailing )
    // Without the std.string INNER hint, the naive scanner would
    // close the scope at the ')' inside the quotes.
    auto v = parse_input(g, "(\"wait)here\" trailing)");
    auto sv = as_string(v);
    REQUIRE(sv);
    CHECK(sv->data() == "\"wait)here\" trailing");
    CHECK(save_value(g, v) == "(\"wait)here\" trailing)");
}

// ─── Recursive scope nesting via self-Ref INNER ─────────────────────────

TEST_CASE("scope: nested () via self-Ref INNER preserves both layers") {
    auto g = load(R"GRAM(
        use: std
        start: <PAREN>
        PAREN: scope { "(", <PAREN>, std.string, ")" }
    )GRAM");
    auto v = parse_input(g, "(outer (inner) tail)");
    auto sv = as_string(v);
    REQUIRE(sv);
    CHECK(sv->data() == "outer (inner) tail");
    CHECK(save_value(g, v) == "(outer (inner) tail)");
}

TEST_CASE("scope: nested scope containing a string with ')'") {
    auto g = load(R"GRAM(
        use: std
        start: <PAREN>
        PAREN: scope { "(", <PAREN>, std.string, ")" }
    )GRAM");
    auto v = parse_input(g, "(outer (\"in)ner\") done)");
    auto sv = as_string(v);
    REQUIRE(sv);
    CHECK(sv->data() == "outer (\"in)ner\") done");
    CHECK(save_value(g, v) == "(outer (\"in)ner\") done)");
}

// ─── Different bracket flavours ─────────────────────────────────────────

TEST_CASE("scope: square brackets work the same as parens") {
    auto g = load(R"GRAM(
        use: std
        start: <SQ>
        SQ: scope { "[", std.string, "]" }
    )GRAM");
    auto v = parse_input(g, "[a \"with]bracket\" b]");
    auto sv = as_string(v);
    REQUIRE(sv);
    CHECK(sv->data() == "a \"with]bracket\" b");
    CHECK(save_value(g, v) == "[a \"with]bracket\" b]");
}
