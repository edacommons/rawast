#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>

#include <memory>
#include <sstream>

using namespace rawast;

TEST_CASE("Grammar allocates nodes with correct kinds") {
    Grammar g;
    NodeId c   = g.new_choice();
    NodeId s   = g.new_sequence();
    NodeId r   = g.new_repeat();
    NodeId k   = g.new_key("foo");
    NodeId p   = g.new_parse("int");
    NodeId rf  = g.new_ref("VALUE");
    NodeId v   = g.new_value(null_value());

    CHECK(g.node(c).kind   == NodeKind::Choice);
    CHECK(g.node(s).kind   == NodeKind::Sequence);
    CHECK(g.node(r).kind   == NodeKind::Repeat);
    CHECK(g.node(k).kind   == NodeKind::Key);
    CHECK(g.node(p).kind   == NodeKind::Parse);
    CHECK(g.node(rf).kind  == NodeKind::Ref);
    CHECK(g.node(v).kind   == NodeKind::Value);
}

TEST_CASE("Grammar add_X attaches children to parent") {
    Grammar g;
    NodeId seq = g.new_sequence();
    g.add_key(seq, "(");
    g.add_parse(seq, "int");
    g.add_key(seq, ")");
    CHECK(g.node(seq).children.size() == 3);
}

TEST_CASE("Grammar set_optional / set_name / set_container") {
    Grammar g;
    NodeId p = g.new_parse("string");
    g.set_optional(p);
    g.set_name(p);
    CHECK(g.node(p).is_optional);
    CHECK(g.node(p).is_name);

    NodeId seq = g.new_sequence();
    g.set_container(seq, Container::Dict);
    CHECK(g.node(seq).container == Container::Dict);
}

TEST_CASE("Grammar set_separator inserts at index 0") {
    Grammar g;
    NodeId rep = g.new_repeat();
    g.add_ref(rep, "VALUE");
    NodeId sep = g.new_key(",");
    g.set_separator(rep, sep);
    CHECK(g.node(rep).has_separator);
    CHECK(g.node(rep).children.size() == 2);
    CHECK(g.node(rep).children[0] == sep);
}

TEST_CASE("Grammar register_rule and resolve_ref follow chains") {
    Grammar g;
    NodeId target = g.new_choice();
    g.register_rule("VALUE", target);

    NodeId ref1 = g.new_ref("VALUE");
    NodeId resolved = g.resolve_ref(ref1);
    CHECK(resolved == target);
}

TEST_CASE("Choice handles shared-prefix alternatives by default") {
    // Build a Choice between two alternatives that share a leading "a":
    //   alt1: "a" "b"
    //   alt2: "a" "c"
    // Backtrack is default-on (standard PEG semantics, matches the
    // LEF/DEF prototype): "ac" parses successfully because alt1 fails on
    // "b" and the engine rewinds to try alt2, which matches.
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("TOP", top);

    NodeId alt1 = g.add_sequence(top);
    g.add_key(alt1, "a");
    g.add_key(alt1, "b");

    NodeId alt2 = g.add_sequence(top);
    g.add_key(alt2, "a");
    g.add_key(alt2, "c");

    g.set_top(g.new_ref("TOP"));

    std::istringstream is{"ac"};
    StreamReader sr{is};
    CHECK(g.parse(sr));
}

TEST_CASE("Choice with backtrack handles shared-prefix alternatives") {
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("TOP", top);
    g.set_backtrack(top);   // <- opt in

    NodeId alt1 = g.add_sequence(top);
    g.add_key(alt1, "a");
    g.add_key(alt1, "b");

    NodeId alt2 = g.add_sequence(top);
    g.add_key(alt2, "a");
    g.add_key(alt2, "c");

    g.set_top(g.new_ref("TOP"));

    SUBCASE("input matches first alternative") {
        std::istringstream is{"ab"};
        StreamReader sr{is};
        CHECK(g.parse(sr));
    }
    SUBCASE("input matches second alternative (requires backtrack)") {
        std::istringstream is{"ac"};
        StreamReader sr{is};
        CHECK(g.parse(sr));
    }
    SUBCASE("input matches no alternative") {
        std::istringstream is{"ad"};
        StreamReader sr{is};
        CHECK_FALSE(g.parse(sr));
    }
}

TEST_CASE("Backtracking Choice rewinds the stream between attempts") {
    // After a failed alternative attempt consumes "a", the stream should
    // be rewound to position 0 before the next alternative starts —
    // otherwise the second alternative would parse starting from "c"
    // instead of "ac".
    Grammar g;
    NodeId top = g.new_choice();
    g.register_rule("TOP", top);
    g.set_backtrack(top);

    NodeId alt1 = g.add_sequence(top);
    g.add_key(alt1, "a");
    g.add_key(alt1, "b");

    NodeId alt2 = g.add_sequence(top);
    g.add_key(alt2, "a");
    g.add_key(alt2, "c");

    g.set_top(g.new_ref("TOP"));

    std::istringstream is{"ac"};
    StreamReader sr{is};
    REQUIRE(g.parse(sr));
    // After successful parse, the stream should be at EOF.
    CHECK(sr.eof());
}

TEST_CASE("Nested backtracking Choices rewind correctly") {
    // Outer choice over two inner choices; inner choices have shared
    // prefixes. Verifies the mark stack manages nested attempts.
    Grammar g;
    NodeId outer = g.new_choice();
    g.register_rule("OUTER", outer);
    g.set_backtrack(outer);

    // Outer alt A: matches "ax" + "b" (i.e. "axb")
    NodeId outer_a = g.add_sequence(outer);
    g.add_key(outer_a, "ax");
    g.add_key(outer_a, "b");

    // Outer alt B: matches "ax" + inner choice("cd" | "ce")
    NodeId outer_b = g.add_sequence(outer);
    g.add_key(outer_b, "ax");
    NodeId inner = g.add_choice(outer_b);
    g.set_backtrack(inner);

    NodeId inner_a = g.add_sequence(inner);
    g.add_key(inner_a, "cd");
    g.add_key(inner_a, "f");

    NodeId inner_b = g.add_sequence(inner);
    g.add_key(inner_b, "cd");
    g.add_key(inner_b, "g");

    g.set_top(g.new_ref("OUTER"));

    SUBCASE("axb works") {
        std::istringstream is{"axb"};
        StreamReader sr{is};
        CHECK(g.parse(sr));
    }
    SUBCASE("axcdg requires backtrack through nested choice") {
        std::istringstream is{"axcdg"};
        StreamReader sr{is};
        CHECK(g.parse(sr));
    }
}

TEST_CASE("Loader recognises the backtrack flag on Choice") {
    Grammar g;
    const char* schema = R"({
        "start": "TOP",
        "TOP": {
            "type": "choice",
            "backtrack": true,
            "items": [
                {"type": "sequence", "items": ["a", "b"]},
                {"type": "sequence", "items": ["a", "c"]}
            ]
        }
    })";
    auto r = load_json_grammar_from_string(g, schema);
    REQUIRE(r);
    NodeId top = g.rule_id("TOP");
    REQUIRE(top.valid());
    CHECK(g.node(top).backtrack);

    std::istringstream is{"ac"};
    StreamReader sr{is};
    CHECK(g.parse(sr));
}

TEST_CASE("Value-kind nodes honour is_name in the catcher absorber") {
    // Build a grammar where a dict-container Sequence has Value-kind
    // children with is_name set — they should emit fixed string names
    // into the catcher, paired with values from sibling Parse children.
    //
    // Equivalent .rawast: a hand-desugared form of `int:value=@`,
    // producing {"type":"integer", "value": <parsed int>}.
    Grammar g;
    g.register_parser(std::make_unique<IntParser>());

    NodeId top = g.new_sequence();
    g.register_rule("TOP", top);
    g.set_container(top, Container::Dict);

    NodeId type_name = g.new_value(make_string("type"));
    g.set_name(type_name);                                  // <- is_name=true on Value
    g.node(top).children.push_back(type_name);

    NodeId type_val = g.new_value(make_string("integer"));
    g.node(top).children.push_back(type_val);

    NodeId val_name = g.new_value(make_string("value"));
    g.set_name(val_name);                                   // <- is_name=true on Value
    g.node(top).children.push_back(val_name);

    NodeId val_parse = g.new_parse("int");
    g.node(top).children.push_back(val_parse);

    g.set_top(g.new_ref("TOP"));

    std::istringstream is{"42"};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    REQUIRE(d->data().size() == 2);
    CHECK(std::dynamic_pointer_cast<StringValue>(d->data().at("type"))->data() == "integer");
    CHECK(std::dynamic_pointer_cast<IntValue>(d->data().at("value"))->data() == 42);
}
