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
    // System task: also round-trips as-is.
    auto u2 = p.unparse(*make_string("$display"));
    REQUIRE(u2);
    CHECK(*u2 == "$display");
    // A name with chars that aren't simple-id valid → escape form
    // with trailing space.
    auto u3 = p.unparse(*make_string("foo.bar"));
    REQUIRE(u3);
    CHECK(*u3 == "\\foo.bar ");
}

// ============================================================
// SvNumberParser
// ============================================================

TEST_CASE("sv: number — unsized integer") {
    SvNumberParser p;
    auto v = try_parse(p, "42");
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    REQUIRE(d);
    auto kind = std::dynamic_pointer_cast<StringValue>(d->data().at("kind"));
    REQUIRE(kind);
    CHECK(kind->data() == "integer");
    auto val = std::dynamic_pointer_cast<IntValue>(d->data().at("value"));
    REQUIRE(val);
    CHECK(val->data() == 42);
}

TEST_CASE("sv: number — integer with underscores") {
    SvNumberParser p;
    auto v = try_parse(p, "1_000_000");
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    auto val = std::dynamic_pointer_cast<IntValue>(d->data().at("value"));
    REQUIRE(val);
    CHECK(val->data() == 1000000);
}

TEST_CASE("sv: number — sized hex") {
    SvNumberParser p;
    auto v = try_parse(p, "8'hFF");
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    auto kind = std::dynamic_pointer_cast<StringValue>(d->data().at("kind"));
    CHECK(kind->data() == "based");
    auto size = std::dynamic_pointer_cast<IntValue>(d->data().at("size"));
    CHECK(size->data() == 8);
    auto sgn = std::dynamic_pointer_cast<BoolValue>(d->data().at("signed"));
    CHECK(sgn->data() == false);
    auto base = std::dynamic_pointer_cast<StringValue>(d->data().at("base"));
    CHECK(base->data() == "h");
    auto digits = std::dynamic_pointer_cast<StringValue>(d->data().at("value"));
    CHECK(digits->data() == "FF");
}

TEST_CASE("sv: number — sized binary with underscores") {
    SvNumberParser p;
    auto v = try_parse(p, "8'b1010_1010");
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    auto digits = std::dynamic_pointer_cast<StringValue>(d->data().at("value"));
    CHECK(digits->data() == "10101010");
}

TEST_CASE("sv: number — signed sized") {
    SvNumberParser p;
    auto v = try_parse(p, "8'sh80");
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    auto sgn = std::dynamic_pointer_cast<BoolValue>(d->data().at("signed"));
    CHECK(sgn->data() == true);
    auto base = std::dynamic_pointer_cast<StringValue>(d->data().at("base"));
    CHECK(base->data() == "h");
}

TEST_CASE("sv: number — unsized based") {
    SvNumberParser p;
    auto v = try_parse(p, "'h42");
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    auto size = d->data().at("size");
    CHECK(size == null_value());
    auto digits = std::dynamic_pointer_cast<StringValue>(d->data().at("value"));
    CHECK(digits->data() == "42");
}

TEST_CASE("sv: number — x and z digits") {
    SvNumberParser p;
    auto v1 = try_parse(p, "4'bxxxx");
    REQUIRE(v1);
    auto d1 = std::dynamic_pointer_cast<DictValue>(v1);
    auto digits1 = std::dynamic_pointer_cast<StringValue>(d1->data().at("value"));
    CHECK(digits1->data() == "xxxx");

    auto v2 = try_parse(p, "8'h??");
    REQUIRE(v2);
    auto d2 = std::dynamic_pointer_cast<DictValue>(v2);
    auto digits2 = std::dynamic_pointer_cast<StringValue>(d2->data().at("value"));
    CHECK(digits2->data() == "??");
}

TEST_CASE("sv: number — real literal") {
    SvNumberParser p;
    auto v = try_parse(p, "42.5");
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    auto kind = std::dynamic_pointer_cast<StringValue>(d->data().at("kind"));
    CHECK(kind->data() == "real");
    auto val = std::dynamic_pointer_cast<RealValue>(d->data().at("value"));
    CHECK(val->data() == doctest::Approx(42.5));
}

TEST_CASE("sv: number — real with exponent") {
    SvNumberParser p;
    auto v1 = try_parse(p, "1.5e10");
    REQUIRE(v1);
    auto d1 = std::dynamic_pointer_cast<DictValue>(v1);
    auto val1 = std::dynamic_pointer_cast<RealValue>(d1->data().at("value"));
    CHECK(val1->data() == doctest::Approx(1.5e10));

    auto v2 = try_parse(p, "1.5E-3");
    REQUIRE(v2);
    auto d2 = std::dynamic_pointer_cast<DictValue>(v2);
    auto val2 = std::dynamic_pointer_cast<RealValue>(d2->data().at("value"));
    CHECK(val2->data() == doctest::Approx(1.5e-3));
}

TEST_CASE("sv: number — time literal") {
    SvNumberParser p;
    auto v1 = try_parse(p, "42ns");
    REQUIRE(v1);
    auto d1 = std::dynamic_pointer_cast<DictValue>(v1);
    auto kind1 = std::dynamic_pointer_cast<StringValue>(d1->data().at("kind"));
    CHECK(kind1->data() == "time");
    auto unit1 = std::dynamic_pointer_cast<StringValue>(d1->data().at("unit"));
    CHECK(unit1->data() == "ns");

    auto v2 = try_parse(p, "1.5us");
    REQUIRE(v2);
    auto d2 = std::dynamic_pointer_cast<DictValue>(v2);
    auto unit2 = std::dynamic_pointer_cast<StringValue>(d2->data().at("unit"));
    CHECK(unit2->data() == "us");
}

TEST_CASE("sv: number — integer followed by identifier") {
    // Verify the parser doesn't accidentally swallow the identifier
    // that follows an unsigned integer (e.g. `42x` is `42` then `x`,
    // not a malformed time literal).
    SvNumberParser p;
    std::istringstream is("42x");
    StreamReader sr{is};
    auto r = p.parse(sr);
    REQUIRE(r);
    // The 'x' should still be on the stream — peek returns 'x'.
    CHECK(sr.peek().value_or('?') == 'x');
}

TEST_CASE("sv: number — unparse round-trip") {
    SvNumberParser p;
    // Integer.
    auto i = std::make_shared<DictValue>();
    i->data()["kind"]  = make_string("integer");
    i->data()["value"] = make_int(42);
    auto u_i = p.unparse(*i);
    REQUIRE(u_i);
    CHECK(*u_i == "42");

    // Based.
    auto b = std::make_shared<DictValue>();
    b->data()["kind"]   = make_string("based");
    b->data()["size"]   = make_int(8);
    b->data()["signed"] = false_value();
    b->data()["base"]   = make_string("h");
    b->data()["value"]  = make_string("FF");
    auto u_b = p.unparse(*b);
    REQUIRE(u_b);
    CHECK(*u_b == "8'hFF");

    // Signed based.
    auto bs = std::make_shared<DictValue>();
    bs->data()["kind"]   = make_string("based");
    bs->data()["size"]   = make_int(8);
    bs->data()["signed"] = true_value();
    bs->data()["base"]   = make_string("h");
    bs->data()["value"]  = make_string("80");
    auto u_bs = p.unparse(*bs);
    REQUIRE(u_bs);
    CHECK(*u_bs == "8'sh80");
}

// Strings and comments are covered by std.string / std.line_comment /
// std.block_comment (see test_parsers.cpp); the SV grammar uses
// those terminals directly via `use: std, sv`. No SV-specific tests
// needed for those forms.
