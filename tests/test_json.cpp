#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/parsers.hpp>

#include <memory>
#include <sstream>

using namespace rawast;

namespace {
tl::expected<ValuePtr, ParseError> parse_json(const Grammar& g, std::string input) {
    auto stream = Stream::from_string(std::move(input));
    return g.parse(stream);
}
} // namespace

TEST_CASE("JSON grammar parses null / true / false") {
    auto g = make_json_grammar();

    auto rn = parse_json(g, "null");
    REQUIRE(rn);
    CHECK((*rn).get() == null_value().get());

    auto rt = parse_json(g, "true");
    REQUIRE(rt);
    CHECK((*rt).get() == true_value().get());

    auto rf = parse_json(g, "false");
    REQUIRE(rf);
    CHECK((*rf).get() == false_value().get());
}

TEST_CASE("JSON grammar parses bare integers") {
    auto g = make_json_grammar();

    auto r = parse_json(g, "42");
    REQUIRE(r);
    auto i = std::dynamic_pointer_cast<IntValue>(*r);
    REQUIRE(i);
    CHECK(i->data() == 42);

    auto r2 = parse_json(g, "-17");
    REQUIRE(r2);
    auto i2 = std::dynamic_pointer_cast<IntValue>(*r2);
    REQUIRE(i2);
    CHECK(i2->data() == -17);
}

TEST_CASE("JSON grammar parses floating-point numbers") {
    auto g = make_json_grammar();

    auto r = parse_json(g, "3.14");
    REQUIRE(r);
    auto f = std::dynamic_pointer_cast<RealValue>(*r);
    REQUIRE(f);
    CHECK(f->data() == doctest::Approx(3.14));

    auto r2 = parse_json(g, "1e-3");
    REQUIRE(r2);
    auto f2 = std::dynamic_pointer_cast<RealValue>(*r2);
    REQUIRE(f2);
    CHECK(f2->data() == doctest::Approx(1e-3));
}

TEST_CASE("JSON grammar parses double-quoted strings") {
    auto g = make_json_grammar();

    auto r = parse_json(g, "\"hello\"");
    REQUIRE(r);
    auto s = std::dynamic_pointer_cast<StringValue>(*r);
    REQUIRE(s);
    CHECK(s->data() == "hello");
}

TEST_CASE("JSON grammar parses empty array") {
    auto g = make_json_grammar();

    auto r = parse_json(g, "[]");
    REQUIRE(r);
    auto arr = std::dynamic_pointer_cast<ArrayValue>(*r);
    REQUIRE(arr);
    CHECK(arr->data().empty());
}

TEST_CASE("JSON grammar parses single-element array") {
    auto g = make_json_grammar();

    auto r = parse_json(g, "[42]");
    REQUIRE(r);
    auto arr = std::dynamic_pointer_cast<ArrayValue>(*r);
    REQUIRE(arr);
    CHECK(arr->data().size() == 1);
    CHECK(arr->data()[0]->type() == ValueType::Int);
}

TEST_CASE("JSON grammar parses multi-element array") {
    auto g = make_json_grammar();

    auto r = parse_json(g, "[1, 2, 3]");
    REQUIRE(r);
    auto arr = std::dynamic_pointer_cast<ArrayValue>(*r);
    REQUIRE(arr);
    CHECK(arr->data().size() == 3);
    CHECK(std::dynamic_pointer_cast<IntValue>(arr->data()[0])->data() == 1);
    CHECK(std::dynamic_pointer_cast<IntValue>(arr->data()[1])->data() == 2);
    CHECK(std::dynamic_pointer_cast<IntValue>(arr->data()[2])->data() == 3);
}

TEST_CASE("JSON grammar parses mixed-type array") {
    auto g = make_json_grammar();

    auto r = parse_json(g, R"([1, "two", null, true, 3.14])");
    REQUIRE(r);
    auto arr = std::dynamic_pointer_cast<ArrayValue>(*r);
    REQUIRE(arr);
    REQUIRE(arr->data().size() == 5);
    CHECK(arr->data()[0]->type() == ValueType::Int);
    CHECK(arr->data()[1]->type() == ValueType::String);
    CHECK(arr->data()[2]->type() == ValueType::Null);
    CHECK(arr->data()[3]->type() == ValueType::Bool);
    CHECK(arr->data()[4]->type() == ValueType::Real);
}

TEST_CASE("JSON grammar parses empty object") {
    auto g = make_json_grammar();

    auto r = parse_json(g, "{}");
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    CHECK(d->data().empty());
}

TEST_CASE("JSON grammar parses single-pair object") {
    auto g = make_json_grammar();

    auto r = parse_json(g, R"({"answer": 42})");
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    REQUIRE(d->data().size() == 1);
    auto v = std::dynamic_pointer_cast<IntValue>(d->data().at("answer"));
    REQUIRE(v);
    CHECK(v->data() == 42);
}

TEST_CASE("JSON grammar parses multi-pair object") {
    auto g = make_json_grammar();

    auto r = parse_json(g, R"({"a": 1, "b": "two", "c": null})");
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    REQUIRE(d->data().size() == 3);
    CHECK(d->data().at("a")->type() == ValueType::Int);
    CHECK(d->data().at("b")->type() == ValueType::String);
    CHECK(d->data().at("c")->type() == ValueType::Null);
}

TEST_CASE("JSON grammar parses nested structures") {
    auto g = make_json_grammar();

    auto r = parse_json(g, R"({"items": [1, 2, {"key": "val"}]})");
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    auto items = std::dynamic_pointer_cast<ArrayValue>(d->data().at("items"));
    REQUIRE(items);
    REQUIRE(items->data().size() == 3);
    auto inner = std::dynamic_pointer_cast<DictValue>(items->data()[2]);
    REQUIRE(inner);
    CHECK(std::dynamic_pointer_cast<StringValue>(inner->data().at("key"))->data() == "val");
}

TEST_CASE("JSON grammar tolerates whitespace") {
    auto g = make_json_grammar();

    auto r = parse_json(g, "  [  1 ,  2 ,  3  ]  ");
    REQUIRE(r);
    auto arr = std::dynamic_pointer_cast<ArrayValue>(*r);
    REQUIRE(arr);
    CHECK(arr->data().size() == 3);
}

TEST_CASE("JSON grammar handles multi-line input") {
    auto g = make_json_grammar();

    auto r = parse_json(g, "{\n  \"a\": 1,\n  \"b\": 2\n}");
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    CHECK(d->data().size() == 2);
}

TEST_CASE("JSON grammar fails on malformed input") {
    auto g = make_json_grammar();

    auto r = parse_json(g, "{,}");
    CHECK_FALSE(r);

    auto r2 = parse_json(g, "[1, 2,");
    CHECK_FALSE(r2);
}

TEST_CASE("JSON grammar parses a mixed-shape smoke-test input") {
    auto g = make_json_grammar();
    // Top-level array exercises ints, floats, exponents, signs,
    // bare-fraction floats (`.3`), nested arrays, and a nested dict.
    auto r = parse_json(g, R"([[123],[2,-4.1,7],[12],{"abc":[-12]},[null,true,false],-.3,.3,-.1e10,1e2,-1e-3])");
    REQUIRE(r);
    auto arr = std::dynamic_pointer_cast<ArrayValue>(*r);
    REQUIRE(arr);
    CHECK(arr->data().size() == 10);
}

// JSONC support — the in-memory JSON grammar is JSONC by construction
// (make_json_grammar registers // and /* */ comment parsers and adds
// both to the ignore list). File-loaded grammars opt into the same
// behaviour explicitly via the top-level "ignore" field; the host API
// never adds comment ignores implicitly. These tests pin the
// out-of-the-box behaviour for the in-memory grammar.

TEST_CASE("JSONC: in-memory JSON grammar accepts // line comments") {
    auto g = make_json_grammar();
    auto r = parse_json(g,
        "// header comment\n"
        "{\n"
        "  \"name\": \"clk\", // inline comment\n"
        "  \"frequency\": 100\n"
        "}\n");
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    CHECK(d->data().size() == 2);
    CHECK(std::dynamic_pointer_cast<StringValue>(d->data().at("name"))->data() == "clk");
    CHECK(std::dynamic_pointer_cast<IntValue>(d->data().at("frequency"))->data() == 100);
}

TEST_CASE("JSONC: in-memory JSON grammar accepts /* */ block comments") {
    auto g = make_json_grammar();
    auto r = parse_json(g,
        "/* top-level\n"
        "   block comment */\n"
        "{ \"a\": /* between */ 1, \"b\": 2 /* trailing */ }");
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    CHECK(d->data().size() == 2);
}

TEST_CASE("JSONC: both comment styles together") {
    auto g = make_json_grammar();
    auto r = parse_json(g,
        "// file header\n"
        "{\n"
        "  /* multi-line\n"
        "     block comment */\n"
        "  \"name\": \"clk\",  // trailing line comment\n"
        "  \"items\": [1, 2 /* inline */, 3]\n"
        "}");
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    CHECK(d->data().size() == 2);
    auto items = std::dynamic_pointer_cast<ArrayValue>(d->data().at("items"));
    REQUIRE(items);
    CHECK(items->data().size() == 3);
}
