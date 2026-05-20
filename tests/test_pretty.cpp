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
    // depth=1, indent_step="  ": tab → "  ", content "X", tail ";", space " ", newline "\n"
    CHECK(out.str() == "  X; \n");
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

    std::istringstream input("[ 42 ]");
    StreamReader sr(input);
    auto r = g.parse(sr);
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

    std::istringstream input("[ 7 ]");
    StreamReader sr(input);
    auto r = g.parse(sr);
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

    std::istringstream input("[1, 2, 3]");
    StreamReader sr(input);
    auto r = g.parse(sr);
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

    std::istringstream input("[1, 2]");
    StreamReader sr(input);
    auto r = g.parse(sr);
    REQUIRE(r);

    // Pretty mode: emits all attrs.
    std::ostringstream pretty_out;
    REQUIRE(g.save(pretty_out, *r, /*pretty=*/true));
    CHECK(pretty_out.str() ==
        "[\n"
        "  1 ,\n"
        "  2 \n"
        "]"
    );

    // Compact mode: skip tab, indent, newline. Keep space and tail (the
    // `space` after each iteration is preserved so the output stays
    // unambiguous for round-trip).
    std::ostringstream compact_out;
    REQUIRE(g.save(compact_out, *r, /*pretty=*/false));
    CHECK(compact_out.str() == "[1 ,2 ]");

    // Round-trip: re-parse compact output, get same AST.
    std::istringstream compact_in(compact_out.str());
    StreamReader compact_sr(compact_in);
    auto compact_r = g.parse(compact_sr);
    REQUIRE(compact_r);
    std::ostringstream compact_out_2;
    REQUIRE(g.save(compact_out_2, *compact_r, /*pretty=*/false));
    CHECK(compact_out_2.str() == compact_out.str());
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
          string:@=, ":" space, <VALUE>
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
    std::istringstream input(R"({"name":"alice","items":[1,2]})");
    StreamReader sr(input);
    auto parsed = g.parse(sr);
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
    std::istringstream pretty_in(pretty);
    StreamReader pretty_sr(pretty_in);
    auto reparsed = g.parse(pretty_sr);
    REQUIRE(reparsed);
    std::ostringstream out2;
    REQUIRE(g.save(out2, *reparsed));
    CHECK(out2.str() == pretty);
}
