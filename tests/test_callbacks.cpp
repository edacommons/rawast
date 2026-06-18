#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/parsers.hpp>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace rawast;

TEST_CASE("Callbacks: on_rule_complete fires with the rule's parsed value") {
    Grammar g;
    g.register_parser(std::make_unique<IntParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");

    NodeId rule = g.new_parse("int");
    g.register_rule("NUM", rule);
    g.set_top(g.new_ref("NUM"));

    int call_count = 0;
    ValuePtr captured;
    g.on_rule_complete("NUM", [&](const ValuePtr& v) {
        ++call_count;
        captured = v;
    });

    auto stream = Stream::from_string("42");
    auto result = g.parse(stream);
    REQUIRE(result);

    CHECK(call_count == 1);
    REQUIRE(captured);
    auto iv = std::dynamic_pointer_cast<IntValue>(captured);
    REQUIRE(iv);
    CHECK(iv->data() == 42);
}

TEST_CASE("Callbacks: multiple callbacks on the same rule all fire in order") {
    Grammar g;
    g.register_parser(std::make_unique<IntParser>());

    NodeId rule = g.new_parse("int");
    g.register_rule("NUM", rule);
    g.set_top(g.new_ref("NUM"));

    std::vector<int> order;
    g.on_rule_complete("NUM", [&](const ValuePtr&) { order.push_back(1); });
    g.on_rule_complete("NUM", [&](const ValuePtr&) { order.push_back(2); });
    g.on_rule_complete("NUM", [&](const ValuePtr&) { order.push_back(3); });

    auto stream = Stream::from_string("7");
    REQUIRE(g.parse(stream));

    REQUIRE(order.size() == 3);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
    CHECK(order[2] == 3);
}

TEST_CASE("Callbacks: replace_parser swaps a terminal for subsequent input") {
    Grammar g;
    g.register_parser(std::make_unique<IdentifierParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");

    NodeId rule = g.new_parse("identifier");
    g.register_rule("ID", rule);

    NodeId pair = g.new_sequence();
    g.set_container(pair, Container::Array);
    g.register_rule("PAIR", pair);
    g.add_ref(pair, "ID");
    g.add_ref(pair, "ID");
    g.set_top(g.new_ref("PAIR"));

    // After the first identifier parses, swap in a parser that
    // additionally allows '/' as a continuation char.
    g.on_rule_complete("ID", [&g](const ValuePtr&) {
        g.replace_parser(std::make_unique<IdentifierParser>("", "/"));
    });

    // First identifier: default rules (no '/'). Second identifier:
    // post-swap rules — '/' allowed.
    auto stream = Stream::from_string("foo bar/baz");
    auto result = g.parse(stream);
    REQUIRE(result);

    auto arr = std::dynamic_pointer_cast<ArrayValue>(*result);
    REQUIRE(arr);
    REQUIRE(arr->data().size() == 2);
    auto s0 = std::dynamic_pointer_cast<StringValue>(arr->data()[0]);
    auto s1 = std::dynamic_pointer_cast<StringValue>(arr->data()[1]);
    REQUIRE(s0);
    REQUIRE(s1);
    CHECK(s0->data() == "foo");
    CHECK(s1->data() == "bar/baz");   // includes the '/' allowed post-swap
}

TEST_CASE("Callbacks: LEF-style preamble — DIVIDERCHAR declaration teaches the identifier parser") {
    Grammar g;
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g.register_parser(std::make_unique<IdentifierParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");

    // DIVIDER_DECL: "DIVIDERCHAR" "<ch>" ";"
    NodeId decl = g.new_sequence();
    g.register_rule("DIVIDER_DECL", decl);
    g.add_key(decl, "DIVIDERCHAR");
    g.add_parse(decl, "string");
    g.add_key(decl, ";");

    // FILE: DIVIDER_DECL then one identifier (which uses the new char set)
    NodeId file = g.new_sequence();
    g.set_container(file, Container::Array);
    g.register_rule("FILE", file);
    g.add_ref(file, "DIVIDER_DECL");
    g.add_parse(file, "identifier");
    g.set_top(g.new_ref("FILE"));

    g.on_rule_complete("DIVIDER_DECL", [&g](const ValuePtr& v) {
        auto sv = std::dynamic_pointer_cast<StringValue>(v);
        REQUIRE(sv);
        g.replace_parser(std::make_unique<IdentifierParser>("", sv->data()));
    });

    auto stream = Stream::from_string(R"(DIVIDERCHAR "/" ; cell_a/inst1/leaf)");
    auto result = g.parse(stream);
    REQUIRE(result);

    auto arr = std::dynamic_pointer_cast<ArrayValue>(*result);
    REQUIRE(arr);
    REQUIRE(arr->data().size() == 2);     // "/", "cell_a/inst1/leaf"
    auto last = std::dynamic_pointer_cast<StringValue>(arr->data()[1]);
    REQUIRE(last);
    CHECK(last->data() == "cell_a/inst1/leaf");
}

TEST_CASE("Callbacks: queued under backtrack, fire only on accepted branch") {
    Grammar g;
    g.register_parser(std::make_unique<IdentifierParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");

    // FIRST_RULE and SECOND_RULE are both `identifier` parses; they
    // exist as distinct named rules so we can register separate
    // callbacks and tell which branch committed.
    NodeId first  = g.new_parse("identifier");
    NodeId second = g.new_parse("identifier");
    g.register_rule("FIRST_RULE",  first);
    g.register_rule("SECOND_RULE", second);

    // TOP: backtrack-choice {
    //   Sequence(<FIRST_RULE>,  "magic1"),
    //   Sequence(<SECOND_RULE>, "magic2"),
    // }
    NodeId top = g.new_choice();
    g.set_backtrack(top);
    g.register_rule("TOP", top);
    NodeId alt1 = g.add_sequence(top);
    g.add_ref(alt1, "FIRST_RULE");
    g.add_key(alt1, "magic1");
    NodeId alt2 = g.add_sequence(top);
    g.add_ref(alt2, "SECOND_RULE");
    g.add_key(alt2, "magic2");
    g.set_top(g.new_ref("TOP"));

    int first_calls = 0;
    int second_calls = 0;
    g.on_rule_complete("FIRST_RULE",  [&](const ValuePtr&) { ++first_calls; });
    g.on_rule_complete("SECOND_RULE", [&](const ValuePtr&) { ++second_calls; });

    // Input: alt1 matches FIRST_RULE="hello" then expects "magic1" but
    // sees "magic2" — fails, rolls back, FIRST_RULE callback DISCARDED.
    // alt2 matches SECOND_RULE="hello" then "magic2" — succeeds,
    // SECOND_RULE callback FIRES once.
    auto stream = Stream::from_string("hello magic2");
    auto result = g.parse(stream);
    REQUIRE(result);

    CHECK(first_calls  == 0);
    CHECK(second_calls == 1);
}

TEST_CASE("Callbacks: nested backtrack — outer accept flushes inner accept; outer reject discards") {
    Grammar g;
    g.register_parser(std::make_unique<IdentifierParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");

    NodeId inner_rule = g.new_parse("identifier");
    g.register_rule("INNER", inner_rule);

    // INNER_CHOICE: backtrack-choice { Sequence(<INNER>, "x"), Sequence(<INNER>, "y") }
    NodeId inner_choice = g.new_choice();
    g.set_backtrack(inner_choice);
    g.register_rule("INNER_CHOICE", inner_choice);
    NodeId i_alt1 = g.add_sequence(inner_choice);
    g.add_ref(i_alt1, "INNER");
    g.add_key(i_alt1, "x");
    NodeId i_alt2 = g.add_sequence(inner_choice);
    g.add_ref(i_alt2, "INNER");
    g.add_key(i_alt2, "y");

    // OUTER: backtrack-choice {
    //   Sequence(<INNER_CHOICE>, "stop"),
    //   Sequence(<INNER_CHOICE>, "go"),
    // }
    NodeId outer = g.new_choice();
    g.set_backtrack(outer);
    g.register_rule("OUTER", outer);
    NodeId o_alt1 = g.add_sequence(outer);
    g.add_ref(o_alt1, "INNER_CHOICE");
    g.add_key(o_alt1, "stop");
    NodeId o_alt2 = g.add_sequence(outer);
    g.add_ref(o_alt2, "INNER_CHOICE");
    g.add_key(o_alt2, "go");
    g.set_top(g.new_ref("OUTER"));

    int inner_calls = 0;
    g.on_rule_complete("INNER", [&](const ValuePtr&) { ++inner_calls; });

    // Input "h y go":
    //   outer alt1 marks. inner_choice marks i_alt1: INNER="h" queued,
    //     then "x" — fail; reject i_alt1. i_alt2 marks: INNER="h"
    //     queued, "y" matches; accept i_alt2 → its queued callback merges
    //     into outer alt1's queue (still under outer mark). Then "stop"
    //     expected — fail; reject outer alt1 → DISCARD all queued.
    //   outer alt2 marks. inner_choice marks i_alt1: INNER="h" queued,
    //     "x" fail; reject. i_alt2: INNER="h" queued, "y" matches;
    //     accept → merge into outer alt2's queue. Then "go" matches;
    //     accept outer alt2 → flush (top-level: fire).
    //   Final inner_calls = 1.
    auto stream = Stream::from_string("h y go");
    auto result = g.parse(stream);
    REQUIRE(result);
    CHECK(inner_calls == 1);
}
