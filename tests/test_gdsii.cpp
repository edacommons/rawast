#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_gdsii.hpp>
#include <rawast/parsers_registry.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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

    // Save round-trip: with name-keyed save for fixed-schema dicts,
    // the LIBRARY dict re-emits in grammar order (version, timestamp,
    // name, units, ENDLIB), not std::map's alphabetical order. The
    // re-parsed AST should match the original.
    std::ostringstream out;
    REQUIRE(g.save(out, *r));
    std::istringstream is2(out.str());
    StreamReader sr2(is2);
    auto r2 = g.parse(sr2);
    REQUIRE(r2);
    auto dict2 = std::dynamic_pointer_cast<DictValue>(*r2);
    REQUIRE(dict2);
    CHECK(std::dynamic_pointer_cast<IntValue>(dict2->data().at("version"))->data() == 5);
    CHECK(std::dynamic_pointer_cast<StringValue>(dict2->data().at("name"))->data() == "MYLIB");
    // Byte-identical round-trip for the minimum library (no element-
    // type Choice, so the name-keyed save covers every field).
    CHECK(out.str() == bytes);
}

TEST_CASE("GDSII: full grammar from grammars/gdsii.rawast — library with a boundary") {
    // Load the standalone grammar file and parse a hand-crafted library
    // containing one structure with one BOUNDARY element. Validates the
    // full nested structure: LIBRARY -> structures[] -> STRUCTURE ->
    //                       elements[] -> ELEMENT (boundary).
    //
    // No explicit register_gdsii_parsers() call — the grammar's
    // `use: gdsii` directive triggers it automatically.
    Grammar g;
    REQUIRE(load_rawast_grammar_from_file(g, "grammars/gdsii.rawast"));

    // Hand-craft a minimal but realistic GDSII byte stream.
    std::string bytes;

    // HEADER version=5
    append_header(bytes, 6, 0x0002);
    append_i16(bytes, 5);

    // BGNLIB timestamp (12 INT16s)
    append_header(bytes, 28, 0x0102);
    for (int i = 0; i < 12; ++i) append_i16(bytes, static_cast<std::int16_t>(i + 1));

    // LIBNAME "MYLIB"
    append_header(bytes, 10, 0x0206);
    bytes += "MYLIB";
    bytes.push_back('\0');

    // UNITS (user, db)
    append_header(bytes, 20, 0x0305);
    bytes += make_real64(0.001);
    bytes += make_real64(1e-9);

    // ---- STRUCTURE "TOP" ----
    // BGNSTR timestamp
    append_header(bytes, 28, 0x0502);
    for (int i = 0; i < 12; ++i) append_i16(bytes, static_cast<std::int16_t>(i + 1));

    // STRNAME "TOP"
    append_header(bytes, 8, 0x0606);
    bytes += "TOP";
    bytes.push_back('\0');

    // ---- BOUNDARY element ----
    // BOUNDARY discriminator (NO_DATA)
    append_header(bytes, 4, 0x0800);
    // LAYER 1
    append_header(bytes, 6, 0x0D02);
    append_i16(bytes, 1);
    // DATATYPE 0
    append_header(bytes, 6, 0x0E02);
    append_i16(bytes, 0);
    // XY — 5 points (square), 10 INT32s = 40 bytes payload
    append_header(bytes, 44, 0x1003);
    const int square[][2] = {{0,0}, {100,0}, {100,100}, {0,100}, {0,0}};
    for (auto& pt : square) {
        std::uint32_t x = static_cast<std::uint32_t>(pt[0]);
        std::uint32_t y = static_cast<std::uint32_t>(pt[1]);
        bytes.push_back(static_cast<char>((x >> 24) & 0xFF));
        bytes.push_back(static_cast<char>((x >> 16) & 0xFF));
        bytes.push_back(static_cast<char>((x >>  8) & 0xFF));
        bytes.push_back(static_cast<char>( x        & 0xFF));
        bytes.push_back(static_cast<char>((y >> 24) & 0xFF));
        bytes.push_back(static_cast<char>((y >> 16) & 0xFF));
        bytes.push_back(static_cast<char>((y >>  8) & 0xFF));
        bytes.push_back(static_cast<char>( y        & 0xFF));
    }
    // ENDEL
    append_header(bytes, 4, 0x1100);

    // ---- end STRUCTURE ----
    append_header(bytes, 4, 0x0700);   // ENDSTR

    // ---- end LIBRARY ----
    append_header(bytes, 4, 0x0400);   // ENDLIB

    // Parse.
    std::istringstream is(bytes);
    StreamReader sr(is);
    auto r = g.parse(sr);
    if (!r) {
        FAIL("parse failed at byte " << r.error().position.bytes
             << ": " << r.error().message);
    }
    REQUIRE(r);

    auto lib = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(lib);

    // Top-level fields.
    CHECK(std::dynamic_pointer_cast<IntValue>(lib->data().at("version"))->data() == 5);
    CHECK(std::dynamic_pointer_cast<StringValue>(lib->data().at("name"))->data() == "MYLIB");

    // structures[0] = TOP
    auto structures = std::dynamic_pointer_cast<ArrayValue>(lib->data().at("structures"));
    REQUIRE(structures);
    REQUIRE(structures->data().size() == 1);
    auto top_struct = std::dynamic_pointer_cast<DictValue>(structures->data()[0]);
    REQUIRE(top_struct);
    CHECK(std::dynamic_pointer_cast<StringValue>(top_struct->data().at("name"))->data() == "TOP");

    // structures[0].elements[0] = the boundary
    auto elements = std::dynamic_pointer_cast<ArrayValue>(top_struct->data().at("elements"));
    REQUIRE(elements);
    REQUIRE(elements->data().size() == 1);
    auto elem = std::dynamic_pointer_cast<DictValue>(elements->data()[0]);
    REQUIRE(elem);
    CHECK(std::dynamic_pointer_cast<StringValue>(elem->data().at("element"))->data() == "boundary");
    CHECK(std::dynamic_pointer_cast<IntValue>(elem->data().at("layer"))->data() == 1);
    CHECK(std::dynamic_pointer_cast<IntValue>(elem->data().at("datatype"))->data() == 0);

    // xy: 10 INT32 values
    auto xy = std::dynamic_pointer_cast<ArrayValue>(elem->data().at("xy"));
    REQUIRE(xy);
    REQUIRE(xy->data().size() == 10);
    CHECK(std::dynamic_pointer_cast<IntValue>(xy->data()[0])->data() == 0);
    CHECK(std::dynamic_pointer_cast<IntValue>(xy->data()[1])->data() == 0);
    CHECK(std::dynamic_pointer_cast<IntValue>(xy->data()[2])->data() == 100);
    CHECK(std::dynamic_pointer_cast<IntValue>(xy->data()[3])->data() == 0);
    CHECK(std::dynamic_pointer_cast<IntValue>(xy->data()[4])->data() == 100);
    CHECK(std::dynamic_pointer_cast<IntValue>(xy->data()[5])->data() == 100);
}

TEST_CASE("Parser registry: gdsii group is registered and applicable") {
    CHECK(parser_group_exists("gdsii"));

    auto names = registered_parser_groups();
    CHECK(std::find(names.begin(), names.end(), "gdsii") != names.end());

    Grammar g;
    auto r = apply_parser_group(g, "gdsii");
    REQUIRE(r);
    // After applying, the gds_header parser must be available.
    CHECK(g.parser("gds_header") != nullptr);
    CHECK(g.parser("gds_endlib") != nullptr);
}

TEST_CASE("Parser registry: unknown group name produces a clear error") {
    Grammar g;
    auto r = apply_parser_group(g, "definitely_not_a_real_group");
    REQUIRE(!r);
    CHECK(r.error().find("not registered") != std::string::npos);
}

TEST_CASE("Parser registry: .rawast use: of unknown group fails at load time") {
    const char* src = R"RAWAST(
        use: definitely_not_a_real_group

        start: <X>
        X: int
    )RAWAST";

    Grammar g;
    auto r = load_rawast_grammar_from_string(g, src);
    REQUIRE(!r);
    CHECK(r.error().find("definitely_not_a_real_group") != std::string::npos);
}
