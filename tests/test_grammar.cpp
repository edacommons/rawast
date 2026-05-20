#include <doctest/doctest.h>
#include <rawast/grammar.hpp>

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
