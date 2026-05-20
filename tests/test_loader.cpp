#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>

#include <memory>
#include <sstream>

using namespace rawast;

namespace {

// Build a Grammar with the standard JSON terminal parsers registered.
Grammar make_json_target() {
    Grammar g;
    g.register_parser(std::make_unique<IntParser>());
    g.register_parser(std::make_unique<FloatParser>());
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");
    return g;
}

ValuePtr parse_with(const Grammar& g, std::string input) {
    std::istringstream is{std::move(input)};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE(r);
    return *r;
}

std::string save_with(const Grammar& g, ValuePtr value) {
    std::ostringstream out;
    auto r = g.save(out, std::move(value));
    REQUIRE(r);
    return out.str();
}

} // namespace

// Loader basics ----------------------------------------------------------

TEST_CASE("Loader builds JSON grammar from inline JSON-grammar text") {
    auto g = make_json_target();
    const char* schema = R"({
        "start": "VALUE",
        "VALUE": {
            "type": "choice",
            "items": [
                {"type": "int"},
                {"type": "string"},
                {"type": "key", "key": "null", "value": null}
            ]
        }
    })";
    auto r = load_json_grammar_from_string(g, schema);
    REQUIRE(r);
    CHECK(g.has_rule("VALUE"));
}

TEST_CASE("Loader: missing 'start' is an error") {
    auto g = make_json_target();
    auto r = load_json_grammar_from_string(g, R"({"X": "foo"})");
    REQUIRE_FALSE(r);
    CHECK(r.error().find("start") != std::string::npos);
}

TEST_CASE("Loader: 'start' referencing undefined rule is an error") {
    auto g = make_json_target();
    auto r = load_json_grammar_from_string(g, R"({"start": "NOPE"})");
    REQUIRE_FALSE(r);
    CHECK(r.error().find("NOPE") != std::string::npos);
}

TEST_CASE("Loader: malformed grammar JSON returns parse error") {
    auto g = make_json_target();
    auto r = load_json_grammar_from_string(g, R"({"start": "X", )");
    REQUIRE_FALSE(r);
}

// Round-trip via loaded grammar ------------------------------------------

TEST_CASE("Loaded JSON grammar parses primitive values") {
    auto g = make_json_target();
    auto load_r = load_json_grammar_from_file(g, "grammars/json.json");
    REQUIRE(load_r);

    auto v = parse_with(g, "42");
    auto i = std::dynamic_pointer_cast<IntValue>(v);
    REQUIRE(i);
    CHECK(i->data() == 42);
}

TEST_CASE("Loaded JSON grammar parses null/true/false") {
    auto g = make_json_target();
    auto load_r = load_json_grammar_from_file(g, "grammars/json.json");
    REQUIRE(load_r);

    CHECK(parse_with(g, "null").get()  == null_value().get());
    CHECK(parse_with(g, "true").get()  == true_value().get());
    CHECK(parse_with(g, "false").get() == false_value().get());
}

TEST_CASE("Loaded JSON grammar parses arrays") {
    auto g = make_json_target();
    auto load_r = load_json_grammar_from_file(g, "grammars/json.json");
    REQUIRE(load_r);

    auto v = parse_with(g, "[1, 2, 3]");
    auto arr = std::dynamic_pointer_cast<ArrayValue>(v);
    REQUIRE(arr);
    CHECK(arr->data().size() == 3);
}

TEST_CASE("Loaded JSON grammar parses dicts") {
    auto g = make_json_target();
    auto load_r = load_json_grammar_from_file(g, "grammars/json.json");
    REQUIRE(load_r);

    auto v = parse_with(g, R"({"a": 1, "b": 2})");
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    REQUIRE(d);
    CHECK(d->data().size() == 2);
    CHECK(std::dynamic_pointer_cast<IntValue>(d->data().at("a"))->data() == 1);
}

TEST_CASE("Loaded JSON grammar parses nested input identically to native") {
    auto g_loaded = make_json_target();
    REQUIRE(load_json_grammar_from_file(g_loaded, "grammars/json.json"));

    auto g_native = make_json_grammar();

    const std::string input = R"({"items":[1,2,{"key":"val"}],"flag":true})";
    auto v_loaded = parse_with(g_loaded, input);
    auto v_native = parse_with(g_native, input);

    // Save both with the loaded grammar to canonical form and compare.
    CHECK(save_with(g_loaded, v_loaded) == save_with(g_loaded, v_native));
}

TEST_CASE("Loaded JSON grammar round-trips to canonical text via its own save") {
    auto g = make_json_target();
    REQUIRE(load_json_grammar_from_file(g, "grammars/json.json"));

    const std::string input = R"({"a":1,"b":[true,false,null]})";
    auto v = parse_with(g, input);
    CHECK(save_with(g, v) == input);
}

TEST_CASE("Loader: file not found yields a useful error") {
    auto g = make_json_target();
    auto r = load_json_grammar_from_file(g, "grammars/does-not-exist.json");
    REQUIRE_FALSE(r);
    CHECK(r.error().find("cannot open") != std::string::npos);
}
