#include <doctest/doctest.h>

#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_registry.hpp>
#include <rawast/to_value.hpp>
#include <rawast/value.hpp>

#include <memory>
#include <sstream>
#include <string>

using namespace rawast;

// Build a grammar that has been wired up with the std parser group and
// loaded from a .json file at `path`. The std group is needed for any
// grammar that uses string/identifier/whitespace/etc terminals.
static Grammar load_with_std(const std::string& path) {
    Grammar g;
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g.register_parser(std::make_unique<IdentifierParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.register_parser(std::make_unique<LineCommentParser>());
    g.register_parser(std::make_unique<BlockCommentParser>());
    g.add_ignore("whitespace");
    g.add_ignore("line_comment");
    g.add_ignore("block_comment");
    REQUIRE(load_json_grammar_from_file(g, path));
    return g;
}

TEST_CASE("to_value: emits a DictValue root") {
    Grammar g = load_with_std("grammars/json.json");
    auto v = to_value(g);
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    REQUIRE(d);
}

TEST_CASE("to_value: includes start, use, and rule entries") {
    Grammar g = load_with_std("grammars/json.json");
    auto v = to_value(g);
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    REQUIRE(d);

    // "start" should be a string holding the start-rule name.
    auto sit = d->data().find("start");
    REQUIRE(sit != d->data().end());
    auto sv = std::dynamic_pointer_cast<StringValue>(sit->second);
    REQUIRE(sv);
    CHECK(!sv->data().empty());

    // The named rules should appear at the top level. JSON grammar
    // names its start rule "VALUE" (per grammars/json.json).
    auto vit = d->data().find("VALUE");
    REQUIRE(vit != d->data().end());
    auto vd = std::dynamic_pointer_cast<DictValue>(vit->second);
    REQUIRE(vd);
}

TEST_CASE("to_value: round-trip — JSON grammar reloads to equivalent shape") {
    // Load the JSON grammar, emit a dict, load that dict into a fresh
    // Grammar, and verify the round-tripped Grammar can parse the
    // same input as the original. This is the load-bearing invariant.
    Grammar g1 = load_with_std("grammars/json.json");

    auto v = to_value(g1);
    REQUIRE(v);

    Grammar g2;
    g2.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g2.register_parser(std::make_unique<IdentifierParser>());
    g2.register_parser(std::make_unique<WhitespaceParser>());
    g2.register_parser(std::make_unique<LineCommentParser>());
    g2.register_parser(std::make_unique<BlockCommentParser>());
    g2.add_ignore("whitespace");
    g2.add_ignore("line_comment");
    g2.add_ignore("block_comment");
    auto r = load_json_grammar_into(g2, *v);
    REQUIRE_MESSAGE(r, "round-trip load failed: " << (r ? "" : r.error()));

    // Parse the same input through both. The original.
    const std::string input = R"({"name": "rawast", "version": 2, "items": [1, 2, 3]})";
    std::istringstream is1(input);
    StreamReader sr1{is1};
    auto p1 = g1.parse(sr1);
    REQUIRE(p1);

    // The round-tripped grammar.
    std::istringstream is2(input);
    StreamReader sr2{is2};
    auto p2 = g2.parse(sr2);
    REQUIRE_MESSAGE(p2, "round-tripped grammar failed to parse: "
                        << (p2 ? "" : p2.error().message));
}

TEST_CASE("to_value: rawast meta-grammar round-trips") {
    // The big one — the self-host case. If `to_value` can take the
    // meta-grammar and produce a dict that re-loads to a working
    // copy of the meta-grammar, the round-trip is correct for every
    // shipped grammar feature (it uses Choice, Sequence, Repeat, Key,
    // strict-Key, Parse, Value, Raw — the whole node taxonomy).
    Grammar g1 = load_with_std("grammars/rawast.json");

    auto v = to_value(g1);
    REQUIRE(v);

    Grammar g2;
    g2.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g2.register_parser(std::make_unique<IdentifierParser>());
    g2.register_parser(std::make_unique<WhitespaceParser>());
    g2.register_parser(std::make_unique<LineCommentParser>());
    g2.register_parser(std::make_unique<BlockCommentParser>());
    g2.add_ignore("whitespace");
    g2.add_ignore("line_comment");
    g2.add_ignore("block_comment");
    auto r = load_json_grammar_into(g2, *v);
    REQUIRE_MESSAGE(r, "meta-grammar round-trip load failed: "
                       << (r ? "" : r.error()));

    // Parse a small .rawast snippet through both copies.
    const std::string snippet =
        "use: std\n"
        "start: <X>\n"
        "X: sequence { repeat+2 int:nums[]=@ separator \",\" }\n";
    std::istringstream is1(snippet);
    StreamReader sr1{is1};
    auto p1 = g1.parse(sr1);
    REQUIRE(p1);

    std::istringstream is2(snippet);
    StreamReader sr2{is2};
    auto p2 = g2.parse(sr2);
    REQUIRE_MESSAGE(p2, "round-tripped meta-grammar failed: "
                        << (p2 ? "" : p2.error().message));
}

TEST_CASE("to_value: preserves repeat+N min count") {
    // Build a tiny grammar by hand that uses repeat with min=3,
    // emit the dict, check `min: 3` is preserved.
    Grammar g;
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g.register_parser(std::make_unique<IdentifierParser>());

    NodeId rep = g.new_repeat();
    g.set_min(rep, 3);
    g.add_parse(rep, "identifier");

    NodeId rule = g.new_sequence();
    g.set_container(rule, Container::Array);
    g.node(rule).children.push_back(rep);

    g.register_rule("LIST", rule);
    g.set_top(g.new_ref("LIST"));

    auto v = to_value(g);
    REQUIRE(v);
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    REQUIRE(d);
    auto list = std::dynamic_pointer_cast<DictValue>(d->data()["LIST"]);
    REQUIRE(list);
    auto items = std::dynamic_pointer_cast<ArrayValue>(list->data()["value"]);
    REQUIRE(items);
    REQUIRE(items->data().size() == 1);
    auto rep_dict = std::dynamic_pointer_cast<DictValue>(items->data()[0]);
    REQUIRE(rep_dict);
    auto mit = rep_dict->data().find("min");
    REQUIRE(mit != rep_dict->data().end());
    auto mv = std::dynamic_pointer_cast<IntValue>(mit->second);
    REQUIRE(mv);
    CHECK(mv->data() == 3);
}

TEST_CASE("to_value: preserves strict-key flag") {
    // Build a grammar with a strict Key, emit the dict, check
    // `type: "strict_key"` is preserved.
    Grammar g;
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g.register_parser(std::make_unique<IdentifierParser>());

    NodeId rule = g.new_choice();
    NodeId k1 = g.add_key(rule, "not");
    g.set_strict(k1);
    g.add_key(rule, "notch");
    g.register_rule("STMT", rule);
    g.set_top(g.new_ref("STMT"));

    auto v = to_value(g);
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    REQUIRE(d);
    auto stmt = std::dynamic_pointer_cast<DictValue>(d->data()["STMT"]);
    REQUIRE(stmt);
    auto items = std::dynamic_pointer_cast<ArrayValue>(stmt->data()["value"]);
    REQUIRE(items);
    REQUIRE(items->data().size() == 2);
    auto k1_dict = std::dynamic_pointer_cast<DictValue>(items->data()[0]);
    REQUIRE(k1_dict);
    auto t1 = std::dynamic_pointer_cast<StringValue>(k1_dict->data()["type"]);
    REQUIRE(t1);
    CHECK(t1->data() == "strict_key");
    auto k2_dict = std::dynamic_pointer_cast<DictValue>(items->data()[1]);
    REQUIRE(k2_dict);
    auto t2 = std::dynamic_pointer_cast<StringValue>(k2_dict->data()["type"]);
    REQUIRE(t2);
    CHECK(t2->data() == "key");
}

TEST_CASE("to_value: preserves rule_ignore on rules with override") {
    Grammar g;
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g.register_parser(std::make_unique<IdentifierParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());

    NodeId rule = g.new_sequence();
    g.add_parse(rule, "identifier");
    g.register_rule("R", rule);
    g.set_top(g.new_ref("R"));
    g.add_rule_ignore("R", {"whitespace"});

    auto v = to_value(g);
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    REQUIRE(d);
    auto r = std::dynamic_pointer_cast<DictValue>(d->data()["R"]);
    REQUIRE(r);
    auto ri = std::dynamic_pointer_cast<ArrayValue>(r->data()["rule_ignore"]);
    REQUIRE(ri);
    REQUIRE(ri->data().size() == 1);
    auto pn = std::dynamic_pointer_cast<StringValue>(ri->data()[0]);
    REQUIRE(pn);
    CHECK(pn->data() == "whitespace");
}
