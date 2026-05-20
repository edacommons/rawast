#include <doctest/doctest.h>
#include <rawast/node.hpp>

using namespace rawast;

TEST_CASE("NodeId default-constructs invalid") {
    NodeId id;
    CHECK_FALSE(id.valid());
}

TEST_CASE("NodeId carries an explicit value") {
    NodeId id{42};
    CHECK(id.valid());
    CHECK(id.value() == 42);
}

TEST_CASE("NodeId equality and ordering") {
    NodeId a{1};
    NodeId b{1};
    NodeId c{2};

    CHECK(a == b);
    CHECK(a != c);
    CHECK(a < c);
}

TEST_CASE("Node default-constructs with sensible defaults") {
    Node n;
    CHECK(n.kind == NodeKind::Sequence);
    CHECK(n.container == Container::None);
    CHECK_FALSE(n.is_name);
    CHECK_FALSE(n.is_optional);
    CHECK_FALSE(n.has_separator);
    CHECK_FALSE(n.value);
    CHECK(n.children.empty());
}

TEST_CASE("Node can be configured and children attached") {
    Node n;
    n.kind = NodeKind::Choice;
    n.container = Container::Dict;
    n.is_optional = true;
    n.children.push_back(NodeId{0});
    n.children.push_back(NodeId{1});
    n.children.push_back(NodeId{2});

    CHECK(n.kind == NodeKind::Choice);
    CHECK(n.container == Container::Dict);
    CHECK(n.is_optional);
    CHECK(n.children.size() == 3);
    CHECK(n.children[0].value() == 0);
    CHECK(n.children[2].value() == 2);
}

TEST_CASE("Node value field carries a typed value") {
    Node key_node;
    key_node.kind = NodeKind::Key;
    key_node.value = make_string("{");

    REQUIRE(key_node.value);
    CHECK(key_node.value->type() == ValueType::String);
}
