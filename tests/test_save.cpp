#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>

#include <memory>
#include <sstream>

using namespace rawast;

namespace {
std::string save_to_string(const Grammar& g, ValuePtr value) {
    std::ostringstream out;
    auto r = g.save(out, std::move(value));
    REQUIRE(r);
    return out.str();
}

ValuePtr parse_to_value(const Grammar& g, std::string input) {
    std::istringstream is{std::move(input)};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE(r);
    return *r;
}
} // namespace

// Direct terminal-parser unparse ------------------------------------------

TEST_CASE("KeyParser::unparse emits the literal token") {
    KeyParser p{"hello"};
    auto r = p.unparse(*null_value());   // value is ignored by KeyParser
    REQUIRE(r);
    CHECK(*r == "hello");
}

TEST_CASE("IntParser::unparse formats positive and negative integers") {
    IntParser p;
    auto r1 = p.unparse(*make_int(42));
    REQUIRE(r1);
    CHECK(*r1 == "42");

    auto r2 = p.unparse(*make_int(-17));
    REQUIRE(r2);
    CHECK(*r2 == "-17");
}

TEST_CASE("UIntParser::unparse formats unsigned integer") {
    UIntParser p;
    auto r = p.unparse(*make_uint(99));
    REQUIRE(r);
    CHECK(*r == "99");
}

TEST_CASE("FloatParser::unparse formats and ensures dot/exp present") {
    FloatParser p;
    auto r1 = p.unparse(*make_real(3.14));
    REQUIRE(r1);
    CHECK(*r1 == "3.14");

    // Integer-valued double must still serialise with a fractional marker
    // (so round-tripping back through FloatParser succeeds).
    auto r2 = p.unparse(*make_real(5.0));
    REQUIRE(r2);
    CHECK((*r2 == "5.0" || *r2 == "5e0"));   // either form is valid
}

TEST_CASE("DoubleQuoteStringParser::unparse wraps in quotes") {
    DoubleQuoteStringParser p;
    auto r = p.unparse(*make_string("hello"));
    REQUIRE(r);
    CHECK(*r == "\"hello\"");
}

TEST_CASE("WhitespaceParser::unparse returns SaveError (no save support)") {
    WhitespaceParser p;
    auto r = p.unparse(*make_string(" "));
    CHECK_FALSE(r);
}

// JSON round-trip via Grammar::save --------------------------------------

TEST_CASE("Round-trip null/true/false") {
    auto g = make_json_grammar();
    CHECK(save_to_string(g, null_value())  == "null");
    CHECK(save_to_string(g, true_value())  == "true");
    CHECK(save_to_string(g, false_value()) == "false");
}

TEST_CASE("Round-trip integers") {
    auto g = make_json_grammar();
    CHECK(save_to_string(g, make_int(42))   == "42");
    CHECK(save_to_string(g, make_int(-17))  == "-17");
    CHECK(save_to_string(g, make_int(0))    == "0");
}

TEST_CASE("Round-trip floats") {
    auto g = make_json_grammar();
    CHECK(save_to_string(g, make_real(3.14)) == "3.14");
}

TEST_CASE("Round-trip strings") {
    auto g = make_json_grammar();
    CHECK(save_to_string(g, make_string("hello")) == "\"hello\"");
    CHECK(save_to_string(g, make_string(""))      == "\"\"");
}

TEST_CASE("Round-trip empty array") {
    auto g = make_json_grammar();
    CHECK(save_to_string(g, make_array()) == "[]");
}

TEST_CASE("Round-trip simple array") {
    auto g = make_json_grammar();
    auto a = std::dynamic_pointer_cast<ArrayValue>(make_array());
    a->data().push_back(make_int(1));
    a->data().push_back(make_int(2));
    a->data().push_back(make_int(3));
    CHECK(save_to_string(g, a) == "[1,2,3]");
}

TEST_CASE("Round-trip mixed-type array") {
    auto g = make_json_grammar();
    auto a = std::dynamic_pointer_cast<ArrayValue>(make_array());
    a->data().push_back(make_int(1));
    a->data().push_back(make_string("two"));
    a->data().push_back(null_value());
    a->data().push_back(true_value());
    CHECK(save_to_string(g, a) == "[1,\"two\",null,true]");
}

TEST_CASE("Round-trip empty dict") {
    auto g = make_json_grammar();
    CHECK(save_to_string(g, make_dict()) == "{}");
}

TEST_CASE("Round-trip single-pair dict") {
    auto g = make_json_grammar();
    auto d = std::dynamic_pointer_cast<DictValue>(make_dict());
    d->data().emplace("answer", make_int(42));
    CHECK(save_to_string(g, d) == "{\"answer\":42}");
}

TEST_CASE("Round-trip nested dict and array") {
    auto g = make_json_grammar();
    auto d = std::dynamic_pointer_cast<DictValue>(make_dict());
    auto a = std::dynamic_pointer_cast<ArrayValue>(make_array());
    a->data().push_back(make_int(1));
    a->data().push_back(make_int(2));
    d->data().emplace("items", a);
    CHECK(save_to_string(g, d) == "{\"items\":[1,2]}");
}

// Parse-then-save round-trips -- the canonical-text claim from §3.3

TEST_CASE("Parse then save: simple primitives round-trip") {
    auto g = make_json_grammar();
    auto v = parse_to_value(g, "42");
    CHECK(save_to_string(g, v) == "42");
}

TEST_CASE("Parse then save: array round-trips") {
    auto g = make_json_grammar();
    auto v = parse_to_value(g, "[1,2,3]");
    CHECK(save_to_string(g, v) == "[1,2,3]");
}

TEST_CASE("Parse then save: dict round-trips") {
    auto g = make_json_grammar();
    auto v = parse_to_value(g, "{\"a\":1}");
    CHECK(save_to_string(g, v) == "{\"a\":1}");
}

TEST_CASE("Parse then save: nested structure round-trips") {
    auto g = make_json_grammar();
    auto v = parse_to_value(g, "{\"items\":[1,2,{\"k\":\"v\"}]}");
    CHECK(save_to_string(g, v) == "{\"items\":[1,2,{\"k\":\"v\"}]}");
}

TEST_CASE("Parse-then-save canonicalizes whitespace away") {
    auto g = make_json_grammar();
    // Input has whitespace; canonical output has none.
    auto v = parse_to_value(g, "  [  1 ,  2 ,  3  ]  ");
    CHECK(save_to_string(g, v) == "[1,2,3]");
}

TEST_CASE("Parse-then-save with JSONC input: comments stripped, canonical output") {
    // make_json_grammar() is JSONC by construction — no extra setup needed.
    auto g = make_json_grammar();
    auto v = parse_to_value(g,
        "// header\n"
        "{ \"a\": 1 /* inline */, \"b\": 2 }");
    CHECK(save_to_string(g, v) == "{\"a\":1,\"b\":2}");
}

// ─── Save dispatcher: separator-as-discriminator ────────────────
// Regression suite for the precedence-ladder chain pattern where
// the discriminator lives on the separator, not on the item:
//
//     CHAIN: sequence dict {
//         repeat+2 <NEXT>:args[]=@ separator '||':op="||"
//     }
//
// The dispatcher must recognise that CHAIN's discriminator is
// `dict[op] == "||"` (set by the separator's literal binding),
// not a catch-all wildcard. Previously the bug:
//   * A top-level Choice between CHAIN and a leaf rule treats
//     CHAIN as the catch-all and dispatches every dict to it,
//     including type-tagged leaves that have nothing to do with
//     CHAIN.
//   * Two parallel chain rules (`OR_CHAIN` with `||`, `AND_CHAIN`
//     with `&&`) become indistinguishable: both dispatch to the
//     first chain alt regardless of the dict's actual op.

TEST_CASE("save: separator-discriminator — chain rule does not eat type-tagged leaves") {
    Grammar g;
    register_std_parser_group();
    const char* src = R"(
        use: std
        start: <TOP>
        TOP ignore whitespace: choice { <CHAIN>, <LEAF> }
        CHAIN: sequence dict {
            repeat+2 <LEAF>:args[]=@ separator '||':op="||"
        }
        LEAF: sequence dict {
            identifier:type="leaf":name=@
        }
    )";
    auto load = load_rawast_grammar_from_string(g, src);
    REQUIRE_MESSAGE(load, "load failed: " << (load ? "" : load.error()));

    // Bare LEAF — no `args`, no `op`. Must dispatch to LEAF, not CHAIN.
    auto leaf_ast = parse_to_value(g, "FOO");
    CHECK(save_to_string(g, leaf_ast) == "FOO");

    // Chain — dispatch to CHAIN, emit operator between args.
    auto chain_ast = parse_to_value(g, "A || B");
    CHECK(save_to_string(g, chain_ast) == "A||B");
}

TEST_CASE("save: separator-discriminator — two chains with different ops dispatch correctly") {
    Grammar g;
    register_std_parser_group();
    const char* src = R"(
        use: std
        start: <OR>
        OR ignore whitespace: choice { <OR_CHAIN>, <AND> }
        OR_CHAIN: sequence dict {
            repeat+2 <AND>:args[]=@ separator '||':op="||"
        }
        AND: choice { <AND_CHAIN>, <LEAF> }
        AND_CHAIN: sequence dict {
            repeat+2 <LEAF>:args[]=@ separator '&&':op="&&"
        }
        LEAF: sequence dict {
            identifier:type="leaf":name=@
        }
    )";
    auto load = load_rawast_grammar_from_string(g, src);
    REQUIRE_MESSAGE(load, "load failed: " << (load ? "" : load.error()));

    // Same surface dict shape ({args, op}), different op — dispatcher
    // must pick the right chain rule via the op discriminator.
    auto or_ast = parse_to_value(g, "A || B");
    CHECK(save_to_string(g, or_ast) == "A||B");

    auto and_ast = parse_to_value(g, "A && B");
    CHECK(save_to_string(g, and_ast) == "A&&B");
}

// ─── Save dispatcher: inline-Choice discriminator ───────────────
// A rule whose body contains an inline `choice { K1:f=v1, K2:f=v2 }`
// is currently treated as a wildcard catch-all because no top-level
// (V-name + Value-const) pair is found. The dispatcher commits to
// it for any dict whose field-presence walk passes — even when
// the inner Choice cannot dispatch — and then errors out at
// emission time with "no matching grammar alternative for value
// at save".
//
// Rule pattern this exercises:
//
//   EQ: choice { EQ_BINOP, LEAF }
//   EQ_BINOP: sequence dict {
//       <LEAF>:args[]=@,
//       choice { '==':op="==", '!=':op="!=" },
//       <LEAF>:args[]=@
//   }
//
// For a dict that does NOT carry `op` in {"==", "!="} (e.g. a bare
// LEAF), EQ_BINOP must be rejected so the LEAF alt can take over.

TEST_CASE("save: inline-Choice discriminator — rule with inner op-Choice rejects non-matching dicts") {
    Grammar g;
    register_std_parser_group();
    // Reproduces the sv_pp_expr round-trip failure shape: a unary
    // rule (NOT_EXPR with op="!") whose AST has the SAME args-list
    // shape as EQ_BINOP, but a different op constant. The dict has
    // `args` so the field-presence check on EQ_BINOP passes; only
    // the inner Choice's op discriminator can tell them apart.
    const char* src = R"(
        use: std
        start: <EQ>
        EQ ignore whitespace: choice { <EQ_BINOP>, <UNARY> }
        EQ_BINOP: sequence dict {
            <LEAF>:args[]=@,
            choice { '==':op="==", '!=':op="!=" },
            <LEAF>:args[]=@
        }
        UNARY: choice { <NOT_EXPR>, <LEAF> }
        NOT_EXPR: sequence dict {
            '!':op="!",
            <LEAF>:args[]=@
        }
        LEAF: sequence dict {
            identifier:type="leaf":name=@
        }
    )";
    auto load = load_rawast_grammar_from_string(g, src);
    REQUIRE_MESSAGE(load, "load failed: " << (load ? "" : load.error()));

    // EQ_BINOP first in catch-all order; field-presence on `args`
    // passes; the inner Choice's op = "==" / "!=" doesn't match
    // dict.op = "!". Without the fix, dispatcher commits to
    // EQ_BINOP, then errors at the inner Choice with
    // "no matching grammar alternative for value at save".
    auto not_ast = parse_to_value(g, "!FOO");
    CHECK(save_to_string(g, not_ast) == "!FOO");

    // Equality still routes correctly (regression guard).
    auto eq_ast = parse_to_value(g, "A == B");
    CHECK(save_to_string(g, eq_ast) == "A==B");

    auto ne_ast = parse_to_value(g, "A != B");
    CHECK(save_to_string(g, ne_ast) == "A!=B");
}
