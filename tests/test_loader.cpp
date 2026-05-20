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

TEST_CASE("Loader recognizes {type:value, var:true} for emitting names") {
    Grammar g;
    g.register_parser(std::make_unique<IntParser>());

    // A grammar whose dict-container Sequence uses Value-kind children
    // with var:true to emit fixed string names. This is the JSON-form
    // desugar of the .rawast `name=@` binding (§4.5 of the format spec).
    const char* schema = R"({
        "start": "TOP",
        "TOP": {
            "type": "sequence",
            "container": "dict",
            "items": [
                {"type": "value", "value": "type",    "var": true},
                {"type": "value", "value": "integer"},
                {"type": "value", "value": "value",   "var": true},
                {"type": "int"}
            ]
        }
    })";
    auto r = load_json_grammar_from_string(g, schema);
    REQUIRE(r);

    std::istringstream is{"42"};
    StreamReader sr{is};
    auto v = g.parse(sr);
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(*v);
    REQUIRE(d);
    REQUIRE(d->data().size() == 2);
    CHECK(std::dynamic_pointer_cast<StringValue>(d->data().at("type"))->data() == "integer");
    CHECK(std::dynamic_pointer_cast<IntValue>(d->data().at("value"))->data() == 42);
}

TEST_CASE("Loader: {type:value, var:true} flags Node's is_name correctly") {
    Grammar g;
    const char* schema = R"({
        "start": "TOP",
        "TOP": {"type": "value", "value": "x", "var": true}
    })";
    auto r = load_json_grammar_from_string(g, schema);
    REQUIRE(r);
    NodeId top = g.rule_id("TOP");
    REQUIRE(top.valid());
    CHECK(g.node(top).kind == NodeKind::Value);
    CHECK(g.node(top).is_name);
}

// Self-host: load the .rawast grammar JSON file ---------------------------

namespace {

// Build a Grammar with the parsers needed by the .rawast grammar.
Grammar make_rawast_target() {
    Grammar g;
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g.register_parser(std::make_unique<IdentifierParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.register_parser(std::make_unique<LineCommentParser>());
    g.register_parser(std::make_unique<BlockCommentParser>());
    g.add_ignore("whitespace");
    g.add_ignore("line_comment");
    g.add_ignore("block_comment");
    return g;
}

} // namespace

TEST_CASE("grammars/rawast.json loads cleanly") {
    auto g = make_rawast_target();
    auto r = load_json_grammar_from_file(g, "grammars/rawast.json");
    REQUIRE(r);
    CHECK(g.has_rule("FILE"));
    CHECK(g.has_rule("RULE_DEF"));
    CHECK(g.has_rule("EXPR"));
    CHECK(g.has_rule("SEQUENCE_EXPR"));
}

TEST_CASE("Loaded .rawast grammar parses a single ref rule") {
    auto g = make_rawast_target();
    REQUIRE(load_json_grammar_from_file(g, "grammars/rawast.json"));

    std::istringstream is{"start: <VALUE>"};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    REQUIRE(d->data().count("start") == 1);
    auto start_val = std::dynamic_pointer_cast<StringValue>(d->data().at("start"));
    REQUIRE(start_val);
    CHECK(start_val->data() == "VALUE");
}

TEST_CASE("Loaded .rawast grammar parses a choice of parser-name expressions") {
    auto g = make_rawast_target();
    REQUIRE(load_json_grammar_from_file(g, "grammars/rawast.json"));

    std::istringstream is{"VALUE: choice { int, float, string }"};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    REQUIRE(d->data().count("VALUE") == 1);

    auto choice_dict = std::dynamic_pointer_cast<DictValue>(d->data().at("VALUE"));
    REQUIRE(choice_dict);
    auto type_v = std::dynamic_pointer_cast<StringValue>(choice_dict->data().at("type"));
    REQUIRE(type_v);
    CHECK(type_v->data() == "choice");

    auto items_v = std::dynamic_pointer_cast<ArrayValue>(choice_dict->data().at("items"));
    REQUIRE(items_v);
    CHECK(items_v->data().size() == 3);
}

TEST_CASE("Loaded .rawast grammar parses a sequence with array container") {
    auto g = make_rawast_target();
    REQUIRE(load_json_grammar_from_file(g, "grammars/rawast.json"));

    std::istringstream is{R"(LIST: sequence array { "[", repeat <VALUE> separator ",", "]" })"};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    REQUIRE(d->data().count("LIST") == 1);

    auto list_dict = std::dynamic_pointer_cast<DictValue>(d->data().at("LIST"));
    REQUIRE(list_dict);
    auto type_v = std::dynamic_pointer_cast<StringValue>(list_dict->data().at("type"));
    REQUIRE(type_v);
    CHECK(type_v->data() == "sequence");
    auto cont_v = std::dynamic_pointer_cast<StringValue>(list_dict->data().at("container"));
    REQUIRE(cont_v);
    CHECK(cont_v->data() == "array");
    REQUIRE(list_dict->data().count("items") == 1);
}

TEST_CASE("Loaded .rawast grammar parses a sequence without container") {
    auto g = make_rawast_target();
    REQUIRE(load_json_grammar_from_file(g, "grammars/rawast.json"));

    std::istringstream is{R"(PAIR: sequence { string, ":", <VALUE> })"};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    auto pair_dict = std::dynamic_pointer_cast<DictValue>(d->data().at("PAIR"));
    REQUIRE(pair_dict);
    // container field should be absent.
    CHECK(pair_dict->data().count("container") == 0);
}

TEST_CASE("End-to-end: .rawast definition produces a working parser") {
    // Build a target grammar with primitive parsers registered, then
    // load a .rawast grammar definition into it. The loaded grammar
    // should then be usable to parse input.
    Grammar target;
    target.register_parser(std::make_unique<IntParser>());
    target.register_parser(std::make_unique<FloatParser>());
    target.register_parser(std::make_unique<DoubleQuoteStringParser>());
    target.register_parser(std::make_unique<WhitespaceParser>());
    target.add_ignore("whitespace");

    const char* rawast_source = R"(
        start: <VALUE>
        VALUE: choice { <LIST>, float, int, string }
        LIST:  sequence array { "[", repeat <VALUE> separator ",", "]" }
    )";

    auto r = load_rawast_grammar_from_string(target, rawast_source);
    REQUIRE(r);

    std::istringstream is{"[1, 2, 3]"};
    StreamReader sr{is};
    auto v = target.parse(sr);
    REQUIRE(v);
    auto arr = std::dynamic_pointer_cast<ArrayValue>(*v);
    REQUIRE(arr);
    CHECK(arr->data().size() == 3);
    CHECK(std::dynamic_pointer_cast<IntValue>(arr->data()[0])->data() == 1);
    CHECK(std::dynamic_pointer_cast<IntValue>(arr->data()[1])->data() == 2);
    CHECK(std::dynamic_pointer_cast<IntValue>(arr->data()[2])->data() == 3);
}

TEST_CASE("End-to-end: .rawast-defined parser also handles mixed types") {
    Grammar target;
    target.register_parser(std::make_unique<IntParser>());
    target.register_parser(std::make_unique<FloatParser>());
    target.register_parser(std::make_unique<DoubleQuoteStringParser>());
    target.register_parser(std::make_unique<WhitespaceParser>());
    target.add_ignore("whitespace");

    const char* rawast_source = R"(
        start: <VALUE>
        VALUE: choice { <LIST>, float, int, string }
        LIST:  sequence array { "[", repeat <VALUE> separator ",", "]" }
    )";

    REQUIRE(load_rawast_grammar_from_string(target, rawast_source));

    std::istringstream is{R"([1, 3.14, "hello"])"};
    StreamReader sr{is};
    auto v = target.parse(sr);
    REQUIRE(v);
    auto arr = std::dynamic_pointer_cast<ArrayValue>(*v);
    REQUIRE(arr);
    REQUIRE(arr->data().size() == 3);
    CHECK(arr->data()[0]->type() == ValueType::Int);
    CHECK(arr->data()[1]->type() == ValueType::Real);
    CHECK(arr->data()[2]->type() == ValueType::String);
}

TEST_CASE("Loaded .rawast grammar parses key-with-null discriminator") {
    auto g = make_rawast_target();
    REQUIRE(load_json_grammar_from_file(g, "grammars/rawast.json"));

    std::istringstream is{R"(NULL_KEY: "null":null)"};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    auto key_dict = std::dynamic_pointer_cast<DictValue>(d->data().at("NULL_KEY"));
    REQUIRE(key_dict);
    auto type_v = std::dynamic_pointer_cast<StringValue>(key_dict->data().at("type"));
    REQUIRE(type_v);
    CHECK(type_v->data() == "key");
    auto key_v = std::dynamic_pointer_cast<StringValue>(key_dict->data().at("key"));
    REQUIRE(key_v);
    CHECK(key_v->data() == "null");
    REQUIRE(key_dict->data().count("value") == 1);
    CHECK(key_dict->data().at("value").get() == null_value().get());
}
