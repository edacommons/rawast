#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>

#include <memory>
#include <sstream>
#include <string>

using namespace rawast;

// --- Engine-level: flags + tail on Nodes ---------------------------------

TEST_CASE("Pretty: tail string emitted after a Key on save") {
    Grammar g;
    NodeId k = g.new_key("X");
    g.set_tail(k, ";");
    g.set_top(k);

    std::ostringstream out;
    REQUIRE(g.save(out, make_string("any")));
    CHECK(out.str() == "X;");
}

TEST_CASE("Pretty: space flag emits ' ' after content") {
    Grammar g;
    NodeId seq = g.new_sequence();
    g.register_rule("TOP", seq);
    NodeId k1 = g.add_key(seq, "A");
    g.set_space(k1);
    g.add_key(seq, "B");
    g.set_top(g.new_ref("TOP"));

    std::ostringstream out;
    REQUIRE(g.save(out, make_string("any")));
    CHECK(out.str() == "A B");
}

TEST_CASE("Pretty: newline flag emits '\\n' after content") {
    Grammar g;
    NodeId seq = g.new_sequence();
    g.register_rule("TOP", seq);
    NodeId k1 = g.add_key(seq, "A");
    g.set_newline(k1);
    g.add_key(seq, "B");
    g.set_top(g.new_ref("TOP"));

    std::ostringstream out;
    REQUIRE(g.save(out, make_string("any")));
    CHECK(out.str() == "A\nB");
}

TEST_CASE("Pretty: tab flag emits depth × indent_step before content") {
    // Sequence with `indent` (depth-bump) wrapping a Key with `tab`.
    Grammar g;
    NodeId outer = g.new_sequence();
    g.register_rule("OUTER", outer);
    g.set_indent(outer);          // body depth = 1
    NodeId k = g.add_key(outer, "X");
    g.set_tab(k);                  // emit 1 × "  " = "  " before X
    g.set_top(g.new_ref("OUTER"));

    std::ostringstream out;
    REQUIRE(g.save(out, make_string("any")));
    CHECK(out.str() == "  X");
}

TEST_CASE("Pretty: nested indent — depth accumulates") {
    Grammar g;
    NodeId outer = g.new_sequence();
    g.register_rule("OUTER", outer);
    g.set_indent(outer);          // depth = 1 inside outer
    NodeId inner = g.add_sequence(outer);
    g.set_indent(inner);          // depth = 2 inside inner
    NodeId k = g.add_key(inner, "X");
    g.set_tab(k);                  // emit "    " (2 × "  ")
    g.set_top(g.new_ref("OUTER"));

    std::ostringstream out;
    REQUIRE(g.save(out, make_string("any")));
    CHECK(out.str() == "    X");
}

TEST_CASE("Pretty: indent_step is configurable per Grammar") {
    Grammar g;
    g.set_indent_step("\t");       // tab character
    NodeId outer = g.new_sequence();
    g.register_rule("OUTER", outer);
    g.set_indent(outer);
    NodeId k = g.add_key(outer, "X");
    g.set_tab(k);
    g.set_top(g.new_ref("OUTER"));

    std::ostringstream out;
    REQUIRE(g.save(out, make_string("any")));
    CHECK(out.str() == "\tX");
}

TEST_CASE("Pretty: emission order is tab, content, tail, space, newline") {
    Grammar g;
    NodeId outer = g.new_sequence();
    g.register_rule("OUTER", outer);
    g.set_indent(outer);
    NodeId k = g.add_key(outer, "X");
    g.set_tab(k);
    g.set_tail(k, ";");
    g.set_space(k);
    g.set_newline(k);
    g.set_top(g.new_ref("OUTER"));

    std::ostringstream out;
    REQUIRE(g.save(out, make_string("any")));
    // depth=1, indent_step="  ": tab → "  ", content "X", tail ";", space
    // " " (trimmed from the line end by the pretty trailing-whitespace
    // pass), newline "\n".
    CHECK(out.str() == "  X;\n");
}

// --- JSON-form loader: pretty-print fields on item dicts -----------------

TEST_CASE("Pretty: JSON grammar with newline/tab/indent fields works end-to-end") {
    Grammar g;
    g.register_parser(std::make_unique<IntParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");

    const char* schema = R"({
      "start": "TOP",
      "TOP": {
        "type": "sequence",
        "items": [
          {"type": "key", "key": "[", "newline": true},
          {"type": "int", "tab": true, "newline": true, "indent": true},
          {"type": "key", "key": "]", "tab": true}
        ]
      }
    })";
    REQUIRE(load_json_grammar_from_string(g, schema));

    auto stream = Stream::from_string("[ 42 ]");
    auto r = g.parse(stream);
    REQUIRE(r);

    std::ostringstream out;
    REQUIRE(g.save(out, *r));
    // [ + "\n"; (indent depth=1) tab "  " + "42" + "\n"; (back to 0) tab "" + "]"
    CHECK(out.str() ==
        "[\n"
        "  42\n"
        "]"
    );
}

TEST_CASE("Pretty: JSON-form tail string is escape-interpreted") {
    Grammar g;
    g.register_parser(std::make_unique<IntParser>());

    const char* schema = R"({
      "start": "TOP",
      "TOP": {
        "type": "key",
        "key": "X",
        "tail": "\\\n"
      }
    })";
    REQUIRE(load_json_grammar_from_string(g, schema));

    std::ostringstream out;
    REQUIRE(g.save(out, make_string("any")));
    CHECK(out.str() == "X\\\n");   // backslash + real newline
}

// --- .rawast loader: postfix attrs ---------------------------------------

TEST_CASE("Pretty: .rawast postfix flags (newline / tab / indent) on items") {
    const char* src = R"RAWAST(
        start: <TOP>

        TOP: sequence {
          "[" newline,
          int tab newline indent,
          "]" tab
        }
    )RAWAST";

    Grammar g;
    g.register_parser(std::make_unique<IntParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");
    REQUIRE(load_rawast_grammar_from_string(g, src));

    auto stream = Stream::from_string("[ 7 ]");
    auto r = g.parse(stream);
    REQUIRE(r);

    std::ostringstream out;
    REQUIRE(g.save(out, *r));
    CHECK(out.str() ==
        "[\n"
        "  7\n"
        "]"
    );
}

TEST_CASE("Pretty: .rawast tail=\"...\" with escape sequences") {
    const char* src = R"RAWAST(
        start: <TOP>

        TOP: sequence {
          "X" tail="\\\n",
          "Y" tail=";"
        }
    )RAWAST";

    Grammar g;
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");
    REQUIRE(load_rawast_grammar_from_string(g, src));

    std::ostringstream out;
    REQUIRE(g.save(out, make_string("any")));
    CHECK(out.str() == "X\\\nY;");
}

TEST_CASE("Pretty: .rawast postfix attrs on repeat item (`repeat int tab indent`)") {
    // Repeat's inner item now goes through ITEM, so postfix attrs work.
    // `indent` on the item (not the repeat) is the unambiguous form:
    // each iteration becomes the depth-bumped scope, and `tab` emits the
    // new depth's indent. Putting attrs after the `separator` clause
    // would be parsed as separator's attrs.
    const char* src = R"RAWAST(
        start: <TOP>

        TOP: sequence array {
          "[" newline,
          repeat int tab indent separator "," newline,
          "]"
        }
    )RAWAST";

    Grammar g;
    g.register_parser(std::make_unique<IntParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");
    REQUIRE(load_rawast_grammar_from_string(g, src));

    auto stream = Stream::from_string("[1, 2, 3]");
    auto r = g.parse(stream);
    REQUIRE(r);

    std::ostringstream out;
    REQUIRE(g.save(out, *r));
    CHECK(out.str() ==
        "[\n"
        "  1,\n"
        "  2,\n"
        "  3]"
    );
}

TEST_CASE("Pretty: pretty=false skips tab/indent/newline; keeps space and tail") {
    const char* src = R"RAWAST(
        start: <TOP>

        TOP: sequence array {
          "[" newline,
          repeat int tab indent space separator "," newline,
          "" newline,
          "]" tab
        }
    )RAWAST";

    Grammar g;
    g.register_parser(std::make_unique<IntParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");
    REQUIRE(load_rawast_grammar_from_string(g, src));

    auto stream = Stream::from_string("[1, 2]");
    auto r = g.parse(stream);
    REQUIRE(r);

    // Pretty mode: emits all attrs.
    std::ostringstream pretty_out;
    REQUIRE(g.save(pretty_out, *r, /*pretty=*/true));
    CHECK(pretty_out.str() ==
        "[\n"
        "  1 ,\n"
        "  2\n"        // trailing space trimmed by the pretty pass
        "]"
    );

    // Compact mode: skip tab, indent, newline. Keep space and tail (the
    // `space` after each iteration is preserved so the output stays
    // unambiguous for round-trip).
    std::ostringstream compact_out;
    REQUIRE(g.save(compact_out, *r, /*pretty=*/false));
    CHECK(compact_out.str() == "[1 ,2 ]");

    // Round-trip: re-parse compact output, get same AST.
    auto compact_sr_stream = Stream::from_string(compact_out.str());
    auto compact_r = g.parse(compact_sr_stream);
    REQUIRE(compact_r);
    std::ostringstream compact_out_2;
    REQUIRE(g.save(compact_out_2, *compact_r, /*pretty=*/false));
    CHECK(compact_out_2.str() == compact_out.str());
}

TEST_CASE("Pretty: skipped optional Sequence with indent/tab emits nothing") {
    // An absent optional Sequence carrying `indent tab` must emit no
    // leading indent (and no tail/space/newline). Before the fix,
    // do_consume ran emit_tab/emit_post around the body BEFORE the body
    // decided to skip the absent optional — so the stray indent stacked
    // onto the next sibling, pushing it one level too deep. Mirrors the
    // LEF `?<PORT_CLASS> indent tab` / `?<LAYER_WIDTH_CMD> indent tab`
    // case that over-indented the following LAYER lines.
    const char* src = R"RAWAST(
        start: <BLOCK>

        BLOCK ignore whitespace: sequence dict {
          "B" space, identifier:name=@ newline,
          ?<CLS> indent tab,
          <ITEMS>:items=@,
          "END" newline
        }

        CLS: sequence { "CLASS" space, identifier:cls=@ space, ";" newline }

        ITEMS: sequence array { repeat <ITEM> indent tab }

        ITEM ignore whitespace: sequence dict { "I" space, identifier:v=@ space, ";" newline }
    )RAWAST";

    Grammar g;
    g.register_parser(std::make_unique<IdentifierParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");
    REQUIRE(load_rawast_grammar_from_string(g, src));

    // CLASS absent: the optional must contribute zero indent, so every
    // ITEM sits at exactly one level (two spaces), not stacked deeper.
    {
        auto stream = Stream::from_string("B b\nI x ;\nI y ;\nEND\n");
        auto parsed = g.parse(stream);
        REQUIRE(parsed);
        std::ostringstream out;
        REQUIRE(g.save(out, *parsed, /*pretty=*/true));
        CHECK(out.str() ==
            "B b\n"
            "  I x ;\n"
            "  I y ;\n"
            "END\n");
    }

    // CLASS present: it indents to one level, items unaffected.
    {
        auto stream = Stream::from_string("B b\nCLASS core ;\nI x ;\nEND\n");
        auto parsed = g.parse(stream);
        REQUIRE(parsed);
        std::ostringstream out;
        REQUIRE(g.save(out, *parsed, /*pretty=*/true));
        CHECK(out.str() ==
            "B b\n"
            "  CLASS core ;\n"
            "  I x ;\n"
            "END\n");
    }
}

TEST_CASE("Save: fixed-schema dict round-trips in grammar order, not alphabetical") {
    // A small fixed-schema dict — two named-field parsers in source
    // order. std::map sorts keys alphabetically (age, name) but the
    // grammar declares them in a different order (name, age).
    // Name-keyed save must emit in GRAMMAR order so the output
    // re-parses to the same AST.
    const char* src = R"RAWAST(
        start: <REC>

        REC: sequence dict {
          string:name=@,
          int:age=@
        }
    )RAWAST";

    Grammar g;
    g.register_parser(std::make_unique<IntParser>());
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");
    REQUIRE(load_rawast_grammar_from_string(g, src));

    auto stream = Stream::from_string(R"("alice" 30)");
    auto parsed = g.parse(stream);
    REQUIRE(parsed);
    auto dict = std::dynamic_pointer_cast<DictValue>(*parsed);
    REQUIRE(dict);
    REQUIRE(dict->data().size() == 2);

    std::ostringstream out;
    REQUIRE(g.save(out, *parsed));
    // Grammar order: name first ("alice"), then age (30) — NOT
    // std::map's alphabetical order which would put age first.
    CHECK(out.str() == R"("alice"30)");

    // Round-trip — the saved output reparses to the same dict.
    auto stream2 = Stream::from_string(out.str());
    auto reparsed = g.parse(stream2);
    REQUIRE(reparsed);
    auto dict2 = std::dynamic_pointer_cast<DictValue>(*reparsed);
    REQUIRE(dict2);
    CHECK(std::dynamic_pointer_cast<StringValue>(dict2->data().at("name"))->data() == "alice");
    CHECK(std::dynamic_pointer_cast<IntValue>(dict2->data().at("age"))->data() == 30);
}

TEST_CASE("Save: fixed-schema dict with optional field — present and absent") {
    // A record with an optional field. When the dict has it, save emits
    // it; when missing, save skips both the name marker and the bound
    // parser (skip-mask machinery).
    const char* src = R"RAWAST(
        start: <REC>

        REC: sequence dict {
          string:name=@,
          ?int:age=@
        }
    )RAWAST";

    Grammar g;
    g.register_parser(std::make_unique<IntParser>());
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");
    REQUIRE(load_rawast_grammar_from_string(g, src));

    // With age present.
    {
        auto stream = Stream::from_string(R"("alice" 30)");
        auto parsed = g.parse(stream);
        REQUIRE(parsed);
        std::ostringstream out;
        REQUIRE(g.save(out, *parsed));
        CHECK(out.str() == R"("alice"30)");
    }

    // With age absent — save should skip the int parser entirely.
    {
        auto stream = Stream::from_string(R"("alice")");
        auto parsed = g.parse(stream);
        REQUIRE(parsed);
        std::ostringstream out;
        REQUIRE(g.save(out, *parsed));
        CHECK(out.str() == R"("alice")");
    }
}

TEST_CASE("Pretty: full JSON pretty grammar in .rawast — round-trip") {
    // The textbook example: a JSON-style grammar written in .rawast
    // with postfix pretty-print hints. Parses compact JSON, emits pretty
    // JSON, re-parses the pretty form back to the same AST shape.
    const char* src = R"RAWAST(
        start: <VALUE>

        VALUE: choice {
          <STRUCT>,
          <LIST>,
          string,
          int
        }

        PAIR: sequence {
          string:=@, ":" space, <VALUE>
        }

        STRUCT: sequence dict {
          "{" newline,
          repeat <PAIR> tab indent separator "," newline,
          "" newline,
          "}" tab
        }

        LIST: sequence array {
          "[" newline,
          repeat <VALUE> tab indent separator "," newline,
          "" newline,
          "]" tab
        }
    )RAWAST";

    Grammar g;
    g.register_parser(std::make_unique<IntParser>());
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");
    REQUIRE(load_rawast_grammar_from_string(g, src));

    // Compact JSON in, pretty JSON out.
    auto stream = Stream::from_string(R"({"name":"alice","items":[1,2]})");
    auto parsed = g.parse(stream);
    REQUIRE(parsed);

    std::ostringstream out;
    REQUIRE(g.save(out, *parsed));
    const std::string pretty = out.str();
    // Dict keys come out alphabetically (std::map ordering) — "items"
    // before "name" — so the round-trip output reorders the input. This
    // is a separate concern from pretty-printing.
    CHECK(pretty ==
        "{\n"
        "  \"items\": [\n"
        "    1,\n"
        "    2\n"
        "  ],\n"
        "  \"name\": \"alice\"\n"
        "}"
    );

    // Round-trip: re-parse the pretty output, re-save, expect identical
    // pretty text (idempotent under save).
    auto pretty_sr_stream = Stream::from_string(pretty);
    auto reparsed = g.parse(pretty_sr_stream);
    REQUIRE(reparsed);
    std::ostringstream out2;
    REQUIRE(g.save(out2, *reparsed));
    CHECK(out2.str() == pretty);
}
