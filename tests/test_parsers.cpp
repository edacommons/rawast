#include <doctest/doctest.h>
#include <rawast/parsers.hpp>

#include <memory>
#include <sstream>

using namespace rawast;

namespace {
template <typename ParserT>
ParseResult parse_text(ParserT& parser, std::string input) {
    std::istringstream is{std::move(input)};
    StreamReader sr{is};
    return parser.parse(sr);
}
} // namespace

// KeyParser ---------------------------------------------------------------

TEST_CASE("KeyParser matches an exact literal") {
    KeyParser p{"null"};
    auto r = parse_text(p, "null");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    CHECK(s->data() == "null");
}

TEST_CASE("KeyParser fails on mismatch") {
    KeyParser p{"null"};
    CHECK_FALSE(parse_text(p, "true"));
}

TEST_CASE("KeyParser fails on partial match") {
    KeyParser p{"null"};
    CHECK_FALSE(parse_text(p, "nul"));
}

TEST_CASE("KeyParser leaves the stream unchanged on failure") {
    std::istringstream is{"true"};
    StreamReader sr{is};
    KeyParser p{"null"};
    auto r = p.parse(sr);
    REQUIRE_FALSE(r);
    CHECK(sr.get() == 't');
}

// IntParser ---------------------------------------------------------

TEST_CASE("IntParser parses positive integer") {
    IntParser p;
    auto r = parse_text(p, "42");
    REQUIRE(r);
    auto i = std::dynamic_pointer_cast<IntValue>(*r);
    REQUIRE(i);
    CHECK(i->data() == 42);
}

TEST_CASE("IntParser parses negative integer") {
    IntParser p;
    auto r = parse_text(p, "-42");
    REQUIRE(r);
    auto i = std::dynamic_pointer_cast<IntValue>(*r);
    REQUIRE(i);
    CHECK(i->data() == -42);
}

TEST_CASE("IntParser fails on bare minus") {
    IntParser p;
    CHECK_FALSE(parse_text(p, "-x"));
}

TEST_CASE("IntParser fails on no digits") {
    IntParser p;
    CHECK_FALSE(parse_text(p, "x"));
}

TEST_CASE("IntParser leaves the stream unchanged on failure") {
    std::istringstream is{"-x"};
    StreamReader sr{is};
    IntParser p;
    auto r = p.parse(sr);
    REQUIRE_FALSE(r);
    CHECK(sr.get() == '-');
}

// UIntParser --------------------------------------------------------------

TEST_CASE("UIntParser parses digits") {
    UIntParser p;
    auto r = parse_text(p, "42");
    REQUIRE(r);
    auto u = std::dynamic_pointer_cast<UIntValue>(*r);
    REQUIRE(u);
    CHECK(u->data() == 42u);
}

TEST_CASE("UIntParser fails on minus") {
    UIntParser p;
    CHECK_FALSE(parse_text(p, "-5"));
}

// FloatParser -------------------------------------------------------------

TEST_CASE("FloatParser parses simple decimal") {
    FloatParser p;
    auto r = parse_text(p, "3.14");
    REQUIRE(r);
    auto v = std::dynamic_pointer_cast<RealValue>(*r);
    REQUIRE(v);
    CHECK(v->data() == doctest::Approx(3.14));
}

TEST_CASE("FloatParser parses negative decimal") {
    FloatParser p;
    auto r = parse_text(p, "-3.14");
    REQUIRE(r);
    auto v = std::dynamic_pointer_cast<RealValue>(*r);
    REQUIRE(v);
    CHECK(v->data() == doctest::Approx(-3.14));
}

TEST_CASE("FloatParser parses leading dot") {
    FloatParser p;
    auto r = parse_text(p, ".5");
    REQUIRE(r);
    auto v = std::dynamic_pointer_cast<RealValue>(*r);
    REQUIRE(v);
    CHECK(v->data() == doctest::Approx(0.5));
}

TEST_CASE("FloatParser parses scientific notation") {
    FloatParser p;
    auto r = parse_text(p, "1e10");
    REQUIRE(r);
    auto v = std::dynamic_pointer_cast<RealValue>(*r);
    REQUIRE(v);
    CHECK(v->data() == doctest::Approx(1e10));
}

TEST_CASE("FloatParser parses negative exponent") {
    FloatParser p;
    auto r = parse_text(p, "1e-3");
    REQUIRE(r);
    auto v = std::dynamic_pointer_cast<RealValue>(*r);
    REQUIRE(v);
    CHECK(v->data() == doctest::Approx(1e-3));
}

TEST_CASE("FloatParser parses combined dot + exponent") {
    FloatParser p;
    auto r = parse_text(p, "-.3e10");
    REQUIRE(r);
    auto v = std::dynamic_pointer_cast<RealValue>(*r);
    REQUIRE(v);
    CHECK(v->data() == doctest::Approx(-3e9));
}

TEST_CASE("FloatParser rejects bare integer (no dot, no exponent)") {
    FloatParser p;
    CHECK_FALSE(parse_text(p, "42"));
}

TEST_CASE("FloatParser fails on bare minus") {
    FloatParser p;
    CHECK_FALSE(parse_text(p, "-x"));
}

// WhitespaceParser --------------------------------------------------------

TEST_CASE("WhitespaceParser consumes a run of spaces") {
    WhitespaceParser p;
    auto r = parse_text(p, "   x");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    CHECK(s->data() == "   ");
}

TEST_CASE("WhitespaceParser consumes mixed whitespace") {
    WhitespaceParser p;
    auto r = parse_text(p, " \t\n y");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    CHECK(s->data() == " \t\n ");
}

TEST_CASE("WhitespaceParser fails on no whitespace") {
    WhitespaceParser p;
    CHECK_FALSE(parse_text(p, "x"));
}

// DoubleQuoteStringParser -------------------------------------------------

TEST_CASE("DoubleQuoteStringParser parses simple string") {
    DoubleQuoteStringParser p;
    auto r = parse_text(p, "\"hello\"");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    CHECK(s->data() == "hello");
}

TEST_CASE("DoubleQuoteStringParser preserves escape sequences verbatim") {
    DoubleQuoteStringParser p;
    auto r = parse_text(p, R"("hello\nworld")");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    // Pass-through mode: \n stays as the two literal chars '\' + 'n'.
    CHECK(s->data() == "hello\\nworld");
}

TEST_CASE("DoubleQuoteStringParser parses escaped quote") {
    DoubleQuoteStringParser p;
    auto r = parse_text(p, R"("a\"b")");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    // Pass-through: contents include the backslash and escaped quote.
    CHECK(s->data() == "a\\\"b");
}

TEST_CASE("DoubleQuoteStringParser fails on missing opening quote") {
    DoubleQuoteStringParser p;
    CHECK_FALSE(parse_text(p, "hello\""));
}

TEST_CASE("DoubleQuoteStringParser fails on unterminated string") {
    DoubleQuoteStringParser p;
    CHECK_FALSE(parse_text(p, "\"hello"));
}

TEST_CASE("DoubleQuoteStringParser parses empty string") {
    DoubleQuoteStringParser p;
    auto r = parse_text(p, "\"\"");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    CHECK(s->data().empty());
}

// LineCommentParser -------------------------------------------------------

TEST_CASE("LineCommentParser matches // to end of line") {
    LineCommentParser p;
    auto r = parse_text(p, "// hello\nnext");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    CHECK(s->data() == "// hello");
}

TEST_CASE("LineCommentParser matches // to EOF") {
    LineCommentParser p;
    auto r = parse_text(p, "// no newline at end");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    CHECK(s->data() == "// no newline at end");
}

TEST_CASE("LineCommentParser matches empty body") {
    LineCommentParser p;
    auto r = parse_text(p, "//\n");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    CHECK(s->data() == "//");
}

TEST_CASE("LineCommentParser fails on single slash") {
    LineCommentParser p;
    CHECK_FALSE(parse_text(p, "/x"));
}

TEST_CASE("LineCommentParser fails on no slash") {
    LineCommentParser p;
    CHECK_FALSE(parse_text(p, "xxx"));
}

// BlockCommentParser ------------------------------------------------------

TEST_CASE("BlockCommentParser matches /* ... */") {
    BlockCommentParser p;
    auto r = parse_text(p, "/* hello */");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    CHECK(s->data() == "/* hello */");
}

TEST_CASE("BlockCommentParser spans multiple lines") {
    BlockCommentParser p;
    auto r = parse_text(p, "/* line1\nline2\nline3 */rest");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    CHECK(s->data() == "/* line1\nline2\nline3 */");
}

TEST_CASE("BlockCommentParser matches empty body") {
    BlockCommentParser p;
    auto r = parse_text(p, "/**/");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    CHECK(s->data() == "/**/");
}

TEST_CASE("BlockCommentParser fails on unterminated") {
    BlockCommentParser p;
    CHECK_FALSE(parse_text(p, "/* no end"));
}

TEST_CASE("BlockCommentParser fails on missing star") {
    BlockCommentParser p;
    CHECK_FALSE(parse_text(p, "// not block"));
}

TEST_CASE("BlockCommentParser fails on no opening slash") {
    BlockCommentParser p;
    CHECK_FALSE(parse_text(p, "* alone"));
}
