#include <doctest/doctest.h>

#include <rawast/parsers_sv.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <memory>
#include <sstream>
#include <string>

using namespace rawast;

namespace {

// Parse `input` through `parser` and return the produced ValuePtr or
// nullptr on failure. Helpful one-liner for the dozens of cases below.
ValuePtr try_parse(Parser& parser, const std::string& input) {
    std::istringstream is(input);
    StreamReader sr{is};
    auto r = parser.parse(sr);
    if (!r) return nullptr;
    return *r;
}

// As above, but require success and pull out a StringValue.
std::string parse_as_string(Parser& parser, const std::string& input) {
    auto v = try_parse(parser, input);
    REQUIRE(v);
    auto sv = std::dynamic_pointer_cast<StringValue>(v);
    REQUIRE(sv);
    return sv->data();
}

} // namespace

// ============================================================
// SvIdentifierParser
// ============================================================

TEST_CASE("sv: identifier — simple form") {
    SvIdentifierParser p;
    CHECK(parse_as_string(p, "clk")            == "clk");
    CHECK(parse_as_string(p, "data_in")        == "data_in");
    CHECK(parse_as_string(p, "_internal")      == "_internal");
    CHECK(parse_as_string(p, "x123")           == "x123");
    CHECK(parse_as_string(p, "i$1")            == "i$1");
    CHECK(parse_as_string(p, "ALL_CAPS_OK")    == "ALL_CAPS_OK");
}

TEST_CASE("sv: identifier — escaped form") {
    SvIdentifierParser p;
    // `\foo bar` — name is "foo", trailing space terminates.
    CHECK(parse_as_string(p, "\\foo ")         == "foo");
    // `\foo.bar+baz!` — name is "foo.bar+baz!" (all printable chars
    // until whitespace, including punctuation that wouldn't be valid
    // in a simple identifier).
    CHECK(parse_as_string(p, "\\foo.bar+baz!\t") == "foo.bar+baz!");
    // Escaped identifier with a single non-whitespace char then EOF —
    // is a degenerate-but-legal case. The terminator can be any
    // whitespace OR EOF (loop exits when peek returns nullopt too).
    CHECK(parse_as_string(p, "\\x\n")          == "x");
}

TEST_CASE("sv: identifier — system task / function") {
    SvIdentifierParser p;
    CHECK(parse_as_string(p, "$display")       == "$display");
    CHECK(parse_as_string(p, "$finish")        == "$finish");
    CHECK(parse_as_string(p, "$bits")          == "$bits");
    CHECK(parse_as_string(p, "$random")        == "$random");
    // Bare `$` not followed by an identifier should fail — that
    // sentinel is reserved by the grammar for other use.
    CHECK(try_parse(p, "$ ")                   == nullptr);
}

TEST_CASE("sv: identifier — rejects digit-leading and EOF") {
    SvIdentifierParser p;
    CHECK(try_parse(p, "123abc") == nullptr);
    CHECK(try_parse(p, "")       == nullptr);
    CHECK(try_parse(p, "+")      == nullptr);
}

TEST_CASE("sv: identifier — unparse round-trip") {
    SvIdentifierParser p;
    // Simple: round-trips as-is.
    auto u1 = p.unparse(*make_string("clk"));
    REQUIRE(u1);
    CHECK(*u1 == "clk");
    // `$`-leading: ALWAYS escaped. System task/function names are
    // sv_system_name's domain; a `$`-leading value at an sv_identifier
    // position can only be an escaped identifier (`\$display `), and
    // emitting it bare would re-tokenize as a system name on reparse.
    auto u2 = p.unparse(*make_string("$display"));
    REQUIRE(u2);
    CHECK(*u2 == "\\$display ");
    // A name with chars that aren't simple-id valid → escape form
    // with trailing space.
    auto u3 = p.unparse(*make_string("foo.bar"));
    REQUIRE(u3);
    CHECK(*u3 == "\\foo.bar ");
    // `$`-prefixed but NOT a valid system name (Yosys emits `\$1`,
    // `\$paramod\...` escaped identifiers, stored without the
    // backslash) → must take the escaped form, not the system-name
    // fast path.
    auto u4 = p.unparse(*make_string("$1"));
    REQUIRE(u4);
    CHECK(*u4 == "\\$1 ");
    auto u5 = p.unparse(*make_string("$paramod\\reg\\BITS=32"));
    REQUIRE(u5);
    CHECK(*u5 == "\\$paramod\\reg\\BITS=32 ");
    // System-name SHAPE but at an identifier position (Yosys
    // `\$signal$263`) — still escaped; bare emit would flip the
    // surrounding expression's Choice from REF to SYSTEM_FUNC.
    auto u6 = p.unparse(*make_string("$signal$263"));
    REQUIRE(u6);
    CHECK(*u6 == "\\$signal$263 ");
}

// ============================================================
// SvBasedDigitsParser
// ============================================================
//
// Consumes only the digit-run portion of based literals. Plain int,
// real, time literals, and based-number structure all live in the
// grammar composing std parsers + this digit-run helper. See
// `grammars/systemverilog.rawast` NUMBER rule and the Python
// integration tests for end-to-end coverage.

TEST_CASE("sv: based_digits — hex digit run") {
    SvBasedDigitsParser p;
    CHECK(parse_as_string(p, "FF")       == "FF");
    CHECK(parse_as_string(p, "deadbeef") == "deadbeef");
    CHECK(parse_as_string(p, "0123ABCD") == "0123ABCD");
}

TEST_CASE("sv: based_digits — binary digit run") {
    SvBasedDigitsParser p;
    CHECK(parse_as_string(p, "1010")     == "1010");
    CHECK(parse_as_string(p, "0000")     == "0000");
}

TEST_CASE("sv: based_digits — x, z, ? unknown digits") {
    SvBasedDigitsParser p;
    CHECK(parse_as_string(p, "xxxx")     == "xxxx");
    CHECK(parse_as_string(p, "zzzz")     == "zzzz");
    CHECK(parse_as_string(p, "ZZZZ")     == "ZZZZ");
    CHECK(parse_as_string(p, "????")     == "????");
    CHECK(parse_as_string(p, "x0z1?F")   == "x0z1?F");
}

TEST_CASE("sv: based_digits — underscores stripped") {
    SvBasedDigitsParser p;
    CHECK(parse_as_string(p, "1_0_0_0") == "1000");
    CHECK(parse_as_string(p, "DEAD_BEEF") == "DEADBEEF");
    CHECK(parse_as_string(p, "1__2___3") == "123");
}

TEST_CASE("sv: based_digits — stops at non-digit-non-underscore") {
    SvBasedDigitsParser p;
    std::istringstream is("FFGH");
    StreamReader sr{is};
    auto r = p.parse(sr);
    REQUIRE(r);
    auto sv = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(sv);
    CHECK(sv->data() == "FF");
    CHECK(sr.peek().value_or('?') == 'G');
}

TEST_CASE("sv: based_digits — rejects empty input") {
    SvBasedDigitsParser p;
    CHECK(try_parse(p, "")  == nullptr);
    CHECK(try_parse(p, "G") == nullptr);   // G is not a valid digit
    CHECK(try_parse(p, "_") == nullptr);   // bare underscore — no digits
}

TEST_CASE("sv: based_digits — unparse round-trip") {
    SvBasedDigitsParser p;
    auto u1 = p.unparse(*make_string("FF"));
    REQUIRE(u1);
    CHECK(*u1 == "FF");
    auto u2 = p.unparse(*make_string("xxxx"));
    REQUIRE(u2);
    CHECK(*u2 == "xxxx");
}

// Strings and comments are covered by std.string / std.line_comment /
// std.block_comment (see test_parsers.cpp); plain integers and reals
// use std.int / std.float. No SV-specific tests needed for those forms.
