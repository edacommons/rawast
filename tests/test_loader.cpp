#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>

#include <memory>
#include <sstream>

using namespace rawast;

namespace {

// Build a target Grammar for loading grammars/json.json. The file
// itself declares `"use": ["std"]` and `"ignore": ["whitespace"]`, so
// the helper just ensures the std parser group is registered globally
// before any `use: std` directive is processed.
Grammar make_json_target() {
    register_std_parser_group();
    return Grammar{};
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
    // `use: [std]` is required for `{"type": "int"}` and
    // `{"type": "string"}` Parse nodes to resolve. Loader now
    // validates that every Parse-node parser name exists at load
    // time, so omitting `use` here would be a clear loader error.
    const char* schema = R"({
        "use": ["std"],
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

// Build a target Grammar for loading grammars/rawast.json. The file
// itself declares `"use": ["std"]` and the ignore list it needs, so
// nothing needs to be pre-registered here — the loader brings the
// std parser group in via the `use:` directive.
//
// `register_std_parser_group()` is idempotent and must be called at
// least once per process before any grammar declaring `use: std` is
// loaded. In the Python binding this happens at module init; in
// C++ unit tests we call it from the test helper.
Grammar make_rawast_target() {
    register_std_parser_group();
    return Grammar{};
}

} // namespace

TEST_CASE("grammars/rawast.json loads cleanly") {
    auto g = make_rawast_target();
    auto r = load_json_grammar_from_file(g, "grammars/rawast.json");
    REQUIRE(r);
    CHECK(g.has_rule("GRAMMAR"));
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
    // Flat-shape ref dict: {"type": "VALUE"}. The loader accepts this
    // alongside the JSON-form `"start": "VALUE"` plain-string shape.
    auto start_dict = std::dynamic_pointer_cast<DictValue>(d->data().at("start"));
    REQUIRE(start_dict);
    auto type_val = std::dynamic_pointer_cast<StringValue>(
        start_dict->data().at("type"));
    REQUIRE(type_val);
    CHECK(type_val->data() == "VALUE");
}

TEST_CASE(".rawast loader: unresolved parser reference errors at load time, not at parse") {
    // Regression test for a SIGSEGV: writing `<X>: sequence { foo:=@ }`
    // where `foo` is not a registered std parser used to be accepted by
    // the loader, then crash the parse engine with a null deref at the
    // assert(p) site in NodeKind::Parse dispatch. The right behaviour is
    // a clear loader-time error naming the offending parser, so the
    // grammar author can fix the typo without reaching the runtime path.
    Grammar target;
    register_std_parser_group();
    const char* bad_source = R"(
        use: std
        start: <X>
        X: sequence { definitely_not_a_real_parser:=@ }
    )";
    auto r = load_rawast_grammar_from_string(target, bad_source);
    REQUIRE_FALSE(r);
    // Error should mention the unknown parser name so the user can
    // search for it directly.
    CHECK(r.error().find("definitely_not_a_real_parser") != std::string::npos);
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

    auto items_v = std::dynamic_pointer_cast<ArrayValue>(choice_dict->data().at("value"));
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
    REQUIRE(list_dict->data().count("value") == 1);
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

TEST_CASE("End-to-end: .rawast var-binding (X:=@) — string as dict key") {
    Grammar target;
    target.register_parser(std::make_unique<IntParser>());
    target.register_parser(std::make_unique<DoubleQuoteStringParser>());
    target.register_parser(std::make_unique<WhitespaceParser>());
    target.add_ignore("whitespace");

    // PAIR uses string:=@ to mark the parsed string as the dict key.
    // STRUCT then catches name/value pairs into a dict.
    const char* rawast_source = R"(
        start: <STRUCT>
        STRUCT: sequence dict { "{", repeat <PAIR> separator ",", "}" }
        PAIR:   sequence { string:=@, ":", int }
    )";
    REQUIRE(load_rawast_grammar_from_string(target, rawast_source));

    std::istringstream is{R"({"a": 1, "b": 2, "c": 3})"};
    StreamReader sr{is};
    auto v = target.parse(sr);
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(*v);
    REQUIRE(d);
    REQUIRE(d->data().size() == 3);
    CHECK(std::dynamic_pointer_cast<IntValue>(d->data().at("a"))->data() == 1);
    CHECK(std::dynamic_pointer_cast<IntValue>(d->data().at("b"))->data() == 2);
    CHECK(std::dynamic_pointer_cast<IntValue>(d->data().at("c"))->data() == 3);
}

TEST_CASE("End-to-end: .rawast full JSON grammar — self-host through bindings") {
    Grammar target;
    target.register_parser(std::make_unique<IntParser>());
    target.register_parser(std::make_unique<FloatParser>());
    target.register_parser(std::make_unique<DoubleQuoteStringParser>());
    target.register_parser(std::make_unique<WhitespaceParser>());
    target.add_ignore("whitespace");

    // The complete JSON grammar, written in .rawast, exercising both
    // bindings (string:=@ for the PAIR) and Key-with-constant
    // discriminators (the null/true/false alternatives).
    const char* json_in_rawast = R"(
        start: <VALUE>
        VALUE: choice {
          <STRUCT>,
          <LIST>,
          string,
          float,
          int,
          "null":null,
          "true":true,
          "false":false
        }
        LIST:   sequence array { "[", repeat <VALUE> separator ",", "]" }
        PAIR:   sequence       { string:=@, ":", <VALUE> }
        STRUCT: sequence dict  { "{", repeat <PAIR> separator ",", "}" }
    )";
    REQUIRE(load_rawast_grammar_from_string(target, json_in_rawast));

    // Smoke test: round-trip the JSON grammar's own torture input.
    std::istringstream is{
        R"({"items":[1, 2, {"k":"v"}], "flag": true, "none": null})"};
    StreamReader sr{is};
    auto v = target.parse(sr);
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(*v);
    REQUIRE(d);
    REQUIRE(d->data().size() == 3);
    CHECK(d->data().at("flag").get()  == true_value().get());
    CHECK(d->data().at("none").get()  == null_value().get());
    auto items = std::dynamic_pointer_cast<ArrayValue>(d->data().at("items"));
    REQUIRE(items);
    CHECK(items->data().size() == 3);
}

TEST_CASE("End-to-end: .rawast named binding (X:name=@) — emits named field") {
    Grammar target;
    target.register_parser(std::make_unique<IntParser>());
    target.register_parser(std::make_unique<DoubleQuoteStringParser>());
    target.register_parser(std::make_unique<WhitespaceParser>());
    target.add_ignore("whitespace");

    // RECORD uses named bindings (n=@, v=@) to emit fixed field names
    // into the surrounding dict catcher. Input "name 42" produces
    // {"n": "name", "v": 42}.
    const char* rawast_source = R"(
        start: <RECORD>
        RECORD: sequence dict { string:n=@, int:v=@ }
    )";
    REQUIRE(load_rawast_grammar_from_string(target, rawast_source));

    std::istringstream is{R"("name" 42)"};
    StreamReader sr{is};
    auto v = target.parse(sr);
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(*v);
    REQUIRE(d);
    REQUIRE(d->data().size() == 2);
    CHECK(std::dynamic_pointer_cast<StringValue>(d->data().at("n"))->data() == "name");
    CHECK(std::dynamic_pointer_cast<IntValue>(d->data().at("v"))->data() == 42);
}

TEST_CASE("End-to-end: .rawast literal binding (X:name=\"literal\") — string constant") {
    // Tier-2 binding RHS: X:name="literal" emits a constant kv pair into
    // the surrounding dict catcher, regardless of X's parsed value. The
    // idiom for discriminator records (e.g. GDS_BOUNDARY whose payload
    // is empty but whose presence labels the surrounding element).
    Grammar target;
    target.register_parser(std::make_unique<IntParser>());
    target.register_parser(std::make_unique<DoubleQuoteStringParser>());
    target.register_parser(std::make_unique<WhitespaceParser>());
    target.add_ignore("whitespace");

    const char* rawast_source = R"(
        start: <RECORD>
        RECORD: sequence dict {
          string:element="boundary",
          int:layer=@
        }
    )";
    REQUIRE(load_rawast_grammar_from_string(target, rawast_source));

    std::istringstream is{R"("ignored" 5)"};
    StreamReader sr{is};
    auto v = target.parse(sr);
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(*v);
    REQUIRE(d);
    REQUIRE(d->data().size() == 2);
    // The parsed "ignored" string is discarded; "boundary" is emitted instead.
    CHECK(std::dynamic_pointer_cast<StringValue>(d->data().at("element"))->data() == "boundary");
    CHECK(std::dynamic_pointer_cast<IntValue>(d->data().at("layer"))->data() == 5);
}

TEST_CASE("End-to-end: .rawast literal binding — int / float / bool / null") {
    Grammar target;
    target.register_parser(std::make_unique<IntParser>());
    target.register_parser(std::make_unique<DoubleQuoteStringParser>());
    target.register_parser(std::make_unique<WhitespaceParser>());
    target.add_ignore("whitespace");

    const char* rawast_source = R"(
        start: <REC>
        REC: sequence dict {
          string:version=5,
          string:scale=3.14,
          string:active=true,
          string:archived=false,
          string:next_id=null,
          int:layer=@
        }
    )";
    REQUIRE(load_rawast_grammar_from_string(target, rawast_source));

    std::istringstream is{R"("a" "b" "c" "d" "e" 7)"};
    StreamReader sr{is};
    auto v = target.parse(sr);
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(*v);
    REQUIRE(d);

    // Int literal.
    auto iv = std::dynamic_pointer_cast<IntValue>(d->data().at("version"));
    REQUIRE(iv);
    CHECK(iv->data() == 5);

    // Float literal.
    auto fv = std::dynamic_pointer_cast<RealValue>(d->data().at("scale"));
    REQUIRE(fv);
    CHECK(fv->data() == doctest::Approx(3.14));

    // Bool literals — global singletons.
    CHECK(d->data().at("active")    == true_value());
    CHECK(d->data().at("archived")  == false_value());

    // Null literal.
    CHECK(d->data().at("next_id")   == null_value());

    // Plain parsed binding still works alongside.
    CHECK(std::dynamic_pointer_cast<IntValue>(d->data().at("layer"))->data() == 7);
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

// ──────────────────────────────────────────────────────────────────────
// Regression tests for engine-correctness fixes that landed alongside
// the SV parse-completeness work. Each test corresponds to a real bug
// that produced silently-wrong ASTs before the fix.
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Loader auto-emits Keys with at-bindings (`'X':name=@`)") {
    // BEFORE FIX: `'aaa':a=@` left V-name "a" orphaned (the Key had no
    // Value-child to provide `@`'s value), so the next emitted value
    // paired with "a" — corrupting the field. With auto-emit, the Key's
    // literal text becomes the binding's value.
    register_std_parser_group();
    Grammar target;
    target.register_parser(std::make_unique<WhitespaceParser>());
    target.register_parser(std::make_unique<IdentifierParser>());
    target.add_ignore("whitespace");

    const char* rawast_source = R"(
        start: <X>
        X: sequence dict { 'aaa':a=@, identifier:name=@ }
    )";
    REQUIRE(load_rawast_grammar_from_string(target, rawast_source));

    std::istringstream is{"aaa foo"};
    StreamReader sr{is};
    auto v = target.parse(sr);
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(*v);
    REQUIRE(d);
    REQUIRE(d->data().count("a") == 1);
    REQUIRE(d->data().count("name") == 1);
    CHECK(std::dynamic_pointer_cast<StringValue>(d->data().at("a"))->data() == "aaa");
    CHECK(std::dynamic_pointer_cast<StringValue>(d->data().at("name"))->data() == "foo");
}

TEST_CASE("Loader wraps optional with at-binding (`?<X>:name=@`) — skipped optional doesn't orphan V-name") {
    // BEFORE FIX: `wrap_optional` only fired when const-bindings were
    // present. For at-binding alone, the V-name "a" was a sibling of
    // the optional Ref. When the optional was skipped (peek-and-skip
    // OR try-and-fail), the V-name stayed in emitted_ and paired with
    // the next sibling's value — producing `{a: <bbb_text>, ...}` or
    // similar corruption. After the fix, V-name + optional expr are
    // wrapped in one Sequence; skipping the optional drops both.
    register_std_parser_group();
    Grammar target;
    target.register_parser(std::make_unique<WhitespaceParser>());
    target.register_parser(std::make_unique<IdentifierParser>());
    target.add_ignore("whitespace");

    const char* rawast_source = R"(
        start: <X>
        X: sequence dict { ?'aaa':a=@, 'bbb':@:type="my_type", identifier:name=@ }
    )";
    REQUIRE(load_rawast_grammar_from_string(target, rawast_source));

    // Case 1: optional ABSENT — `a` field should not appear.
    {
        std::istringstream is{"bbb foo"};
        StreamReader sr{is};
        auto v = target.parse(sr);
        REQUIRE(v);
        auto d = std::dynamic_pointer_cast<DictValue>(*v);
        REQUIRE(d);
        CHECK(d->data().count("a") == 0);
        CHECK(d->data().count("name") == 1);
        CHECK(d->data().count("type") == 1);
        CHECK(std::dynamic_pointer_cast<StringValue>(d->data().at("name"))->data() == "foo");
        CHECK(std::dynamic_pointer_cast<StringValue>(d->data().at("type"))->data() == "my_type");
    }
    // Case 2: optional PRESENT — `a` is "aaa" (NOT "bbb" from the next field).
    {
        std::istringstream is{"aaa bbb foo"};
        StreamReader sr{is};
        auto v = target.parse(sr);
        REQUIRE(v);
        auto d = std::dynamic_pointer_cast<DictValue>(*v);
        REQUIRE(d);
        REQUIRE(d->data().count("a") == 1);
        CHECK(std::dynamic_pointer_cast<StringValue>(d->data().at("a"))->data() == "aaa");
        CHECK(std::dynamic_pointer_cast<StringValue>(d->data().at("name"))->data() == "foo");
        CHECK(std::dynamic_pointer_cast<StringValue>(d->data().at("type"))->data() == "my_type");
    }
}

TEST_CASE("Repeat per-iteration mark — failing iteration rolls back to where it started") {
    // BEFORE FIX: an iteration that succeeded partway (e.g. an inner
    // Choice committed) but then failed on a later required item left
    // the cursor advanced. The surrounding rule's continuation
    // (`'END'` here) then failed because the cursor was past `END`.
    // After the fix, each iteration is bracketed in a mark — failing
    // rolls back to the iteration's start.
    register_std_parser_group();
    Grammar target;
    target.register_parser(std::make_unique<WhitespaceParser>());
    target.register_parser(std::make_unique<IdentifierParser>());
    target.add_ignore("whitespace");

    const char* rawast_source = R"(
        start: <TOP>
        TOP: sequence dict { <ITEMS>:items=@, 'END' }
        ITEMS: sequence array { repeat <ITEM> }
        ITEM: sequence dict { <X>:x=@, identifier:y=@, ";" }
        X: choice { 'INT':@, <ID_WRAP> }
        ID_WRAP: sequence { identifier }
    )";
    REQUIRE(load_rawast_grammar_from_string(target, rawast_source));

    // For input `INT x; END`:
    //   * iter 1: ITEM matches `INT x;` (X picks 'INT' alt; rest follows).
    //   * iter 2: ITEM tries `END`. X falls through to ID_WRAP which
    //     matches `END` as an identifier. Then ITEM expects another
    //     identifier (`y`); EOF. ITEM fails.
    //   * Without the rollback fix, the cursor stays past `END` and
    //     TOP's `'END'` literal fails. With the fix, cursor restores to
    //     before `END` and TOP completes with exactly 1 successful iter.
    std::istringstream is{"INT x; END"};
    StreamReader sr{is};
    auto v = target.parse(sr);
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(*v);
    REQUIRE(d);
    auto items = std::dynamic_pointer_cast<ArrayValue>(d->data().at("items"));
    REQUIRE(items);
    CHECK(items->data().size() == 1);
    auto item0 = std::dynamic_pointer_cast<DictValue>(items->data()[0]);
    REQUIRE(item0);
    CHECK(std::dynamic_pointer_cast<StringValue>(item0->data().at("x"))->data() == "INT");
    CHECK(std::dynamic_pointer_cast<StringValue>(item0->data().at("y"))->data() == "x");
}

TEST_CASE("First-byte set propagates through leading optionals in a Sequence") {
    // BEFORE FIX: `?<X>, <Y>` had first-byte = first(X) only. The
    // engine's peek-and-skip then wrongly rejected inputs starting
    // with Y's first byte. After the fix, an optional Ref's nullable
    // bit propagates so the Sequence walker continues accumulating
    // first-byte sets from later children.
    register_std_parser_group();
    Grammar target;
    target.register_parser(std::make_unique<WhitespaceParser>());
    target.register_parser(std::make_unique<IdentifierParser>());
    target.add_ignore("whitespace");

    // CONTAINER repeats ITEM. ITEM is a Choice with one alt
    // (PROP) whose first non-optional child has a distinct first byte
    // (`int`/`bool`) that's NOT in any earlier optional's first-byte
    // set. Without the fix, peek-and-skip rejects valid input.
    const char* rawast_source = R"(
        start: <CONTAINER>
        CONTAINER: sequence dict { 'C':@:type="c", '{', <ITEMS>:items=@, '}' }
        ITEMS: sequence array { repeat <ITEM> }
        ITEM: choice { <PROP> }
        PROP: sequence dict {
            ?'local':vis=@,
            ?'static':life=@,
            <KIND>:kind=@:type="prop",
            identifier:name=@,
            ';'
        }
        KIND: choice { 'int':@, 'bool':@ }
    )";
    REQUIRE(load_rawast_grammar_from_string(target, rawast_source));

    // `int x;` should match PROP even though `i` isn't in either of
    // the leading optionals' first-byte sets ({l}, {s}). With the fix,
    // PROP's first-byte set is {l, s, i, b}.
    std::istringstream is{"C { int x; bool y; }"};
    StreamReader sr{is};
    auto v = target.parse(sr);
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(*v);
    REQUIRE(d);
    auto items = std::dynamic_pointer_cast<ArrayValue>(d->data().at("items"));
    REQUIRE(items);
    CHECK(items->data().size() == 2);
}
