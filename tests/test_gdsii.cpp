#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_gdsii.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

using namespace rawast;

namespace {

// Append a GDSII record header to `out`: 16-bit BE size, 16-bit BE rec_id.
void append_header(std::string& out, std::uint16_t size, std::uint16_t rec_id) {
    out.push_back(static_cast<char>((size   >> 8) & 0xFF));
    out.push_back(static_cast<char>( size         & 0xFF));
    out.push_back(static_cast<char>((rec_id >> 8) & 0xFF));
    out.push_back(static_cast<char>( rec_id       & 0xFF));
}

// 16-bit BE signed.
void append_i16(std::string& out, std::int16_t v) {
    auto u = static_cast<std::uint16_t>(v);
    out.push_back(static_cast<char>((u >> 8) & 0xFF));
    out.push_back(static_cast<char>(u & 0xFF));
}

// GDSII REAL64 encoder — independently reproduces what the parser does
// in reverse. Tests round-trip against this.
std::string make_real64(double v) {
    std::string out(8, '\0');
    if (v == 0.0) return out;
    std::uint8_t b0 = 0;
    if (v < 0) { b0 = 0x80; v = -v; }
    int exp_bin;
    double m_frac = std::frexp(v, &exp_bin);
    std::uint64_t m_int = static_cast<std::uint64_t>(std::ldexp(m_frac, 56));
    int adj_exp = exp_bin - 56;
    while (adj_exp & 3) { ++adj_exp; m_int >>= 1; }
    int E = (adj_exp + 312) / 4;
    b0 |= static_cast<std::uint8_t>(E & 0x7F);
    out[0] = static_cast<char>(b0);
    for (int i = 7; i >= 1; --i) {
        out[i] = static_cast<char>(m_int & 0xFF);
        m_int >>= 8;
    }
    return out;
}

} // anonymous namespace

TEST_CASE("GDSII: single record parse — HEADER (INT16)") {
    Grammar g;
    register_gdsii_parsers(g);
    // Top is just the header parser.
    NodeId top = g.new_parse("gds_header");
    g.set_top(top);

    // HEADER record: size=6, rec_id=0x0002, payload=[INT16 value 5].
    std::string bytes;
    append_header(bytes, 6, 0x0002);
    append_i16(bytes, 5);

    std::istringstream is(bytes);
    StreamReader sr(is);
    auto r = g.parse(sr);
    REQUIRE(r);
    auto iv = std::dynamic_pointer_cast<IntValue>(*r);
    REQUIRE(iv);
    CHECK(iv->data() == 5);
}

TEST_CASE("GDSII: STR record decodes string with trailing null trim") {
    Grammar g;
    register_gdsii_parsers(g);
    NodeId top = g.new_parse("gds_libname");
    g.set_top(top);

    // LIBNAME "MYLIB" — 5 chars + 1 pad = 6 bytes payload, total 10.
    std::string bytes;
    append_header(bytes, 10, 0x0206);   // 0x02 = LIBNAME, 0x06 = STR
    bytes += "MYLIB";
    bytes.push_back('\0');

    std::istringstream is(bytes);
    StreamReader sr(is);
    auto r = g.parse(sr);
    REQUIRE(r);
    auto sv = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(sv);
    CHECK(sv->data() == "MYLIB");
}

TEST_CASE("GDSII: NO_DATA record decodes to null_value") {
    Grammar g;
    register_gdsii_parsers(g);
    NodeId top = g.new_parse("gds_endlib");
    g.set_top(top);

    std::string bytes;
    append_header(bytes, 4, 0x0400);   // 0x04 = ENDLIB, 0x00 = NO_DATA

    std::istringstream is(bytes);
    StreamReader sr(is);
    auto r = g.parse(sr);
    REQUIRE(r);
    CHECK(*r == null_value());
}

TEST_CASE("GDSII: REAL64 round-trip via encoder/decoder") {
    // Encode a known double, parse it through the parser, expect the
    // original value back. This validates both the encode and decode
    // paths against each other.
    Grammar g;
    register_gdsii_parsers(g);
    NodeId top = g.new_parse("gds_mag");   // MAG is REAL64
    g.set_top(top);

    const double original = 0.001;        // a typical GDSII units value
    std::string bytes;
    append_header(bytes, 12, 0x1B05);     // 0x1B = MAG, 0x05 = REAL64
    bytes += make_real64(original);

    std::istringstream is(bytes);
    StreamReader sr(is);
    auto r = g.parse(sr);
    REQUIRE(r);
    auto rv = std::dynamic_pointer_cast<RealValue>(*r);
    REQUIRE(rv);
    CHECK(rv->data() == doctest::Approx(original));
}

TEST_CASE("GDSII: unparse produces wire-identical bytes (round-trip)") {
    Grammar g;
    register_gdsii_parsers(g);

    // Build a record's bytes, parse, save, expect identical bytes back.
    // Use LIBNAME because it's STR — exercises trailing-null padding.
    std::string original;
    append_header(original, 10, 0x0206);
    original += "MYLIB";
    original.push_back('\0');

    NodeId top = g.new_parse("gds_libname");
    g.set_top(top);

    std::istringstream is(original);
    StreamReader sr(is);
    auto parsed = g.parse(sr);
    REQUIRE(parsed);

    std::ostringstream out;
    REQUIRE(g.save(out, *parsed));
    CHECK(out.str() == original);
}

TEST_CASE("GDSII: minimal library parses end-to-end via .rawast grammar") {
    // The minimum well-formed GDSII library: HEADER, BGNLIB, LIBNAME,
    // UNITS, ENDLIB. All five records, hand-crafted, parsed through a
    // small .rawast grammar.

    Grammar g;
    register_gdsii_parsers(g);

    const char* grammar_src = R"RAWAST(
        start: <LIBRARY>

        LIBRARY: sequence dict {
          gds_header:version=@,
          gds_bgnlib:timestamp=@,
          gds_libname:name=@,
          gds_units:units=@,
          gds_endlib
        }
    )RAWAST";
    REQUIRE(load_rawast_grammar_from_string(g, grammar_src));

    // Build the GDSII bytes.
    std::string bytes;
    // HEADER (size=6, rec=0x0002): version=5
    append_header(bytes, 6, 0x0002);
    append_i16(bytes, 5);

    // BGNLIB (size=28, rec=0x0102): 12 INT16 timestamp fields (mod+access)
    append_header(bytes, 28, 0x0102);
    for (int i = 0; i < 12; ++i) append_i16(bytes, static_cast<std::int16_t>(i + 1));

    // LIBNAME (size=10, rec=0x0206): "MYLIB"
    append_header(bytes, 10, 0x0206);
    bytes += "MYLIB";
    bytes.push_back('\0');

    // UNITS (size=20, rec=0x0305): two REAL64s — user_unit, db_unit
    append_header(bytes, 20, 0x0305);
    bytes += make_real64(0.001);
    bytes += make_real64(1e-9);

    // ENDLIB (size=4, rec=0x0400): no payload
    append_header(bytes, 4, 0x0400);

    // Parse.
    std::istringstream is(bytes);
    StreamReader sr(is);
    auto r = g.parse(sr);
    if (!r) {
        FAIL("parse failed at byte " << r.error().position.bytes
             << ": " << r.error().message);
    }
    REQUIRE(r);
    auto dict = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(dict);

    // Verify each field landed in the dict under the expected key.
    CHECK(std::dynamic_pointer_cast<IntValue>(dict->data().at("version"))->data() == 5);

    auto ts = std::dynamic_pointer_cast<ArrayValue>(dict->data().at("timestamp"));
    REQUIRE(ts);
    REQUIRE(ts->data().size() == 12);
    CHECK(std::dynamic_pointer_cast<IntValue>(ts->data()[0])->data() == 1);
    CHECK(std::dynamic_pointer_cast<IntValue>(ts->data()[11])->data() == 12);

    CHECK(std::dynamic_pointer_cast<StringValue>(dict->data().at("name"))->data() == "MYLIB");

    auto units = std::dynamic_pointer_cast<ArrayValue>(dict->data().at("units"));
    REQUIRE(units);
    REQUIRE(units->data().size() == 2);
    CHECK(std::dynamic_pointer_cast<RealValue>(units->data()[0])->data()
          == doctest::Approx(0.001));
    CHECK(std::dynamic_pointer_cast<RealValue>(units->data()[1])->data()
          == doctest::Approx(1e-9));

    // NOTE: Save round-trip for fixed-schema dicts has a known
    // limitation — the save direction iterates the dict in std::map's
    // alphabetical order, which doesn't match the source-ordered
    // grammar children. Implementing name-keyed save for fixed-schema
    // dicts is a follow-on task. Parse direction is fully functional.
}
