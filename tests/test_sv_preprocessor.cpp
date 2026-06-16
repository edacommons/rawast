// Tests for the SV preprocessor grammar's `\`define` rule + segmented
// body. Verifies parse → AST shape and parse → save round-trip.
//
// `"\n"` in the grammar source unescapes to a real newline byte
// thanks to the loader's Key-literal unescape pass — see
// `unescape_key_literal` in src/loader.cpp.

#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/value.hpp>

#include <sstream>

using namespace rawast;

namespace {

Grammar load_grammar() {
    register_std_parser_group();
    Grammar g;
    auto r = load_rawast_grammar_from_file(g, "grammars/sv_preprocessor.rawast");
    REQUIRE_MESSAGE(r, "loading sv_preprocessor.rawast failed: "
                       << (r ? "" : r.error()));
    return g;
}

ValuePtr parse(Grammar& g, const std::string& src) {
    std::istringstream is{src};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE_MESSAGE(r, "parse failed for '" << src << "': "
                       << (r ? "" : r.error().message));
    return *r;
}

std::string save(Grammar& g, ValuePtr v) {
    std::ostringstream out;
    auto r = g.save(out, std::move(v));
    REQUIRE_MESSAGE(r, "save failed: " << (r ? "" : r.error().message));
    return out.str();
}

std::string str_field(const ValuePtr& v, const std::string& key) {
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    if (!d) return {};
    auto it = d->data().find(key);
    if (it == d->data().end()) return {};
    auto s = as_string(it->second);
    return s ? s->data() : std::string{};
}

std::shared_ptr<ArrayValue> body_of(const ValuePtr& ast) {
    auto d = std::dynamic_pointer_cast<DictValue>(ast);
    REQUIRE(d);
    auto it = d->data().find("body");
    REQUIRE(it != d->data().end());
    return std::dynamic_pointer_cast<ArrayValue>(it->second);
}

} // namespace

// ─── Top-level shape ──────────────────────────────────────────────

TEST_CASE("sv_pp define: empty body → {type:'define', name, body:[]}") {
    auto g = load_grammar();
    auto ast = parse(g, "`define FOO\n");
    CHECK(str_field(ast, "type") == "define");
    CHECK(str_field(ast, "name") == "FOO");
    auto body = body_of(ast);
    REQUIRE(body);
    CHECK(body->data().empty());
}

TEST_CASE("sv_pp define: simple text body → one StringValue segment") {
    auto g = load_grammar();
    auto ast = parse(g, "`define FOO bar\n");
    CHECK(str_field(ast, "name") == "FOO");
    auto body = body_of(ast);
    REQUIRE(body);
    // PARAM_REF picks up the identifier `bar` as a typed segment.
    REQUIRE(body->data().size() == 1);
    auto seg = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg);
    CHECK(str_field(seg, "type") == "ref");
    CHECK(str_field(seg, "value") == "bar");
}

TEST_CASE("sv_pp define: param-like identifier followed by text") {
    auto g = load_grammar();
    auto ast = parse(g, "`define FOO x + 1\n");
    auto body = body_of(ast);
    REQUIRE(body);
    // Layout: {ref:x}, " + 1"   — text gap after PARAM_REF coalesces
    REQUIRE(body->data().size() == 2);
    auto seg0 = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg0);
    CHECK(str_field(seg0, "type") == "ref");
    CHECK(str_field(seg0, "value") == "x");
    auto seg1 = as_string(body->data()[1]);
    REQUIRE(seg1);
    CHECK(seg1->data() == " + 1");
}

TEST_CASE("sv_pp define: string literal inside body is atomic") {
    auto g = load_grammar();
    auto ast = parse(g, "`define MSG \"hello world\"\n");
    auto body = body_of(ast);
    REQUIRE(body);
    REQUIRE(body->data().size() == 1);
    auto seg = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg);
    CHECK(str_field(seg, "type") == "string");
    CHECK(str_field(seg, "value") == "hello world");
}

TEST_CASE("sv_pp define: macro_use inside body emits typed segment") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `B\n");
    auto body = body_of(ast);
    REQUIRE(body);
    REQUIRE(body->data().size() == 1);
    auto seg = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg);
    CHECK(str_field(seg, "type") == "macro_use");
    CHECK(str_field(seg, "name") == "B");
}

TEST_CASE("sv_pp define: mixed body — ref + text + string + macro_use") {
    auto g = load_grammar();
    auto ast = parse(g, "`define COMBO x = \"v\" + `OTHER\n");
    auto body = body_of(ast);
    REQUIRE(body);
    // Layout: {ref:x}, " = ", {string:"v"}, " + ", {macro_use:OTHER}
    REQUIRE(body->data().size() == 5);
    auto s0 = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(s0); CHECK(str_field(s0, "type") == "ref");
    CHECK(str_field(s0, "value") == "x");
    auto s1 = as_string(body->data()[1]);
    REQUIRE(s1); CHECK(s1->data() == " = ");
    auto s2 = std::dynamic_pointer_cast<DictValue>(body->data()[2]);
    REQUIRE(s2); CHECK(str_field(s2, "type") == "string");
    CHECK(str_field(s2, "value") == "v");
    auto s3 = as_string(body->data()[3]);
    REQUIRE(s3); CHECK(s3->data() == " + ");
    auto s4 = std::dynamic_pointer_cast<DictValue>(body->data()[4]);
    REQUIRE(s4); CHECK(str_field(s4, "type") == "macro_use");
    CHECK(str_field(s4, "name") == "OTHER");
}

// ─── Round-trip ────────────────────────────────────────────────────

TEST_CASE("sv_pp define: empty body round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define FOO\n");
    // Canonical save emits the identifier's trailing `space` even on
    // empty bodies — semantically equivalent to the parse input.
    CHECK(save(g, ast) == "`define FOO \n");
}

TEST_CASE("sv_pp define: simple body round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define FOO bar\n");
    CHECK(save(g, ast) == "`define FOO bar\n");
}

TEST_CASE("sv_pp define: string body round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define MSG \"hello world\"\n");
    CHECK(save(g, ast) == "`define MSG \"hello world\"\n");
}

TEST_CASE("sv_pp define: macro_use body round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `B\n");
    CHECK(save(g, ast) == "`define A `B\n");
}

TEST_CASE("sv_pp define: mixed body round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define COMBO x = \"v\" + `OTHER\n");
    CHECK(save(g, ast) == "`define COMBO x = \"v\" + `OTHER\n");
}
