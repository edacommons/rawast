// SystemVerilog preprocessor `\`if` expression subset.
//
// Coverage:
//   * Every leaf type (macro_ref, int_lit, call, paren) parses to the
//     documented AST shape.
//   * Every operator (||, &&, ==, !=, !) produces the {op, args[]}
//     shape, with n-ary collapse for || and &&.
//   * Precedence: `A || B && C` parses as `A || (B && C)`, not
//     `(A || B) && C`.
//   * `defined(EXPR)` parses as a generic call — the grammar does NOT
//     enforce that the arg is a macro_ref; that's the host evaluator's
//     job per the contract.

#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/value.hpp>

#include <sstream>

using namespace rawast;

namespace {

Grammar load_grammar() {
    register_std_parser_group();
    Grammar g;
    auto r = load_rawast_grammar_from_file(g, "grammars/sv_pp_expr.rawast");
    REQUIRE_MESSAGE(r, "loading sv_pp_expr.rawast failed: "
                       << (r ? "" : r.error()));
    return g;
}

ValuePtr parse(Grammar& g, const std::string& input) {
    std::istringstream is{input};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE_MESSAGE(r, "parse failed for input '" << input << "': "
                       << (r ? "" : r.error().message));
    return *r;
}

// Pull a string field out of a dict (or empty if missing/wrong type).
std::string str_field(const ValuePtr& v, const std::string& key) {
    auto d = as_dict(v);
    if (!d) return {};
    auto it = d->data().find(key);
    if (it == d->data().end()) return {};
    auto s = as_string(it->second);
    return s ? s->data() : std::string{};
}

ValuePtr value_field(const ValuePtr& v, const std::string& key) {
    auto d = as_dict(v);
    if (!d) return nullptr;
    auto it = d->data().find(key);
    return it == d->data().end() ? nullptr : it->second;
}

std::size_t args_size(const ValuePtr& v) {
    auto args = value_field(v, "args");
    auto a = as_array(args);
    return a ? a->data().size() : 0;
}

ValuePtr arg_at(const ValuePtr& v, std::size_t i) {
    auto args = value_field(v, "args");
    auto a = as_array(args);
    REQUIRE(a);
    REQUIRE(i < a->data().size());
    return a->data()[i];
}

} // namespace

// ─── Leaves ─────────────────────────────────────────────────────

TEST_CASE("sv_pp_expr: bare identifier → {type:'macro_ref', name:...}") {
    auto g = load_grammar();
    auto ast = parse(g, "FOO");
    CHECK(str_field(ast, "type") == "macro_ref");
    CHECK(str_field(ast, "name") == "FOO");
}

TEST_CASE("sv_pp_expr: integer literal → {type:'int_lit', value:42} as IntValue") {
    auto g = load_grammar();
    auto ast = parse(g, "42");
    CHECK(str_field(ast, "type") == "int_lit");
    auto v = value_field(ast, "value");
    REQUIRE(v);
    auto i = std::dynamic_pointer_cast<IntValue>(v);
    REQUIRE_MESSAGE(i, "int_lit value should be IntValue");
    CHECK(i->data() == 42);
}

TEST_CASE("sv_pp_expr: paren expression → {type:'paren', inner:<expr>}") {
    auto g = load_grammar();
    auto ast = parse(g, "(FOO)");
    CHECK(str_field(ast, "type") == "paren");
    auto inner = value_field(ast, "inner");
    REQUIRE(inner);
    CHECK(str_field(inner, "type") == "macro_ref");
    CHECK(str_field(inner, "name") == "FOO");
}

// ─── Calls ──────────────────────────────────────────────────────

TEST_CASE("sv_pp_expr: defined(FOO) → generic call form") {
    auto g = load_grammar();
    auto ast = parse(g, "defined(FOO)");
    CHECK(str_field(ast, "type") == "call");
    CHECK(str_field(ast, "name") == "defined");
    REQUIRE(args_size(ast) == 1);
    auto arg0 = arg_at(ast, 0);
    CHECK(str_field(arg0, "type") == "macro_ref");
    CHECK(str_field(arg0, "name") == "FOO");
}

TEST_CASE("sv_pp_expr: defined(FOO || BAR) — grammar does NOT validate, accepts nested expr") {
    auto g = load_grammar();
    auto ast = parse(g, "defined(FOO || BAR)");
    CHECK(str_field(ast, "type") == "call");
    CHECK(str_field(ast, "name") == "defined");
    REQUIRE(args_size(ast) == 1);
    auto arg0 = arg_at(ast, 0);
    // Per "host validates calls" — the grammar happily parses this.
    // The host evaluator is responsible for rejecting non-macro_ref
    // args at defined-call time.
    CHECK(str_field(arg0, "op") == "||");
    REQUIRE(args_size(arg0) == 2);
}

TEST_CASE("sv_pp_expr: call with multiple args (e.g. some_pred(A, B, C))") {
    auto g = load_grammar();
    auto ast = parse(g, "some_pred(A, B, C)");
    CHECK(str_field(ast, "type") == "call");
    CHECK(str_field(ast, "name") == "some_pred");
    REQUIRE(args_size(ast) == 3);
    CHECK(str_field(arg_at(ast, 0), "name") == "A");
    CHECK(str_field(arg_at(ast, 1), "name") == "B");
    CHECK(str_field(arg_at(ast, 2), "name") == "C");
}

// ─── Unary ──────────────────────────────────────────────────────

TEST_CASE("sv_pp_expr: !FOO → {op:'!', args:[<FOO>]}") {
    auto g = load_grammar();
    auto ast = parse(g, "!FOO");
    CHECK(str_field(ast, "op") == "!");
    REQUIRE(args_size(ast) == 1);
    auto inner = arg_at(ast, 0);
    CHECK(str_field(inner, "type") == "macro_ref");
    CHECK(str_field(inner, "name") == "FOO");
}

TEST_CASE("sv_pp_expr: !!FOO chains unary correctly") {
    auto g = load_grammar();
    auto ast = parse(g, "!!FOO");
    CHECK(str_field(ast, "op") == "!");
    REQUIRE(args_size(ast) == 1);
    auto inner = arg_at(ast, 0);
    CHECK(str_field(inner, "op") == "!");
    REQUIRE(args_size(inner) == 1);
    CHECK(str_field(arg_at(inner, 0), "name") == "FOO");
}

// ─── Binary ─────────────────────────────────────────────────────

TEST_CASE("sv_pp_expr: A || B → n-ary {op:'||', args:[A, B]}") {
    auto g = load_grammar();
    auto ast = parse(g, "A || B");
    CHECK(str_field(ast, "op") == "||");
    REQUIRE(args_size(ast) == 2);
    CHECK(str_field(arg_at(ast, 0), "name") == "A");
    CHECK(str_field(arg_at(ast, 1), "name") == "B");
}

TEST_CASE("sv_pp_expr: A || B || C collapses to flat args[3]") {
    auto g = load_grammar();
    auto ast = parse(g, "A || B || C");
    CHECK(str_field(ast, "op") == "||");
    REQUIRE(args_size(ast) == 3);
    CHECK(str_field(arg_at(ast, 0), "name") == "A");
    CHECK(str_field(arg_at(ast, 1), "name") == "B");
    CHECK(str_field(arg_at(ast, 2), "name") == "C");
}

TEST_CASE("sv_pp_expr: A && B && C && D collapses to flat args[4]") {
    auto g = load_grammar();
    auto ast = parse(g, "A && B && C && D");
    CHECK(str_field(ast, "op") == "&&");
    REQUIRE(args_size(ast) == 4);
}

TEST_CASE("sv_pp_expr: A == B → binary equality") {
    auto g = load_grammar();
    auto ast = parse(g, "A == B");
    CHECK(str_field(ast, "op") == "==");
    REQUIRE(args_size(ast) == 2);
    CHECK(str_field(arg_at(ast, 0), "name") == "A");
    CHECK(str_field(arg_at(ast, 1), "name") == "B");
}

TEST_CASE("sv_pp_expr: A != 0 → binary inequality with int literal") {
    auto g = load_grammar();
    auto ast = parse(g, "A != 0");
    CHECK(str_field(ast, "op") == "!=");
    REQUIRE(args_size(ast) == 2);
    CHECK(str_field(arg_at(ast, 0), "name") == "A");
    auto rhs = arg_at(ast, 1);
    CHECK(str_field(rhs, "type") == "int_lit");
}

// ─── Precedence ─────────────────────────────────────────────────

TEST_CASE("sv_pp_expr: && binds tighter than || — A || B && C parses as A || (B && C)") {
    auto g = load_grammar();
    auto ast = parse(g, "A || B && C");
    REQUIRE(str_field(ast, "op") == "||");
    REQUIRE(args_size(ast) == 2);
    CHECK(str_field(arg_at(ast, 0), "name") == "A");
    auto rhs = arg_at(ast, 1);
    CHECK(str_field(rhs, "op") == "&&");
    REQUIRE(args_size(rhs) == 2);
}

TEST_CASE("sv_pp_expr: == binds tighter than && — A && B == C parses as A && (B == C)") {
    auto g = load_grammar();
    auto ast = parse(g, "A && B == C");
    REQUIRE(str_field(ast, "op") == "&&");
    REQUIRE(args_size(ast) == 2);
    CHECK(str_field(arg_at(ast, 0), "name") == "A");
    auto rhs = arg_at(ast, 1);
    CHECK(str_field(rhs, "op") == "==");
}

TEST_CASE("sv_pp_expr: ! binds tighter than == — !A == B parses as (!A) == B") {
    auto g = load_grammar();
    auto ast = parse(g, "!A == B");
    REQUIRE(str_field(ast, "op") == "==");
    REQUIRE(args_size(ast) == 2);
    auto lhs = arg_at(ast, 0);
    CHECK(str_field(lhs, "op") == "!");
    CHECK(str_field(arg_at(ast, 1), "name") == "B");
}

TEST_CASE("sv_pp_expr: parens override precedence — (A || B) && C") {
    auto g = load_grammar();
    auto ast = parse(g, "(A || B) && C");
    REQUIRE(str_field(ast, "op") == "&&");
    REQUIRE(args_size(ast) == 2);
    auto lhs = arg_at(ast, 0);
    // Top of LHS is a paren wrapper per "keep paren in AST"
    CHECK(str_field(lhs, "type") == "paren");
    auto inner = value_field(lhs, "inner");
    REQUIRE(inner);
    CHECK(str_field(inner, "op") == "||");
}

// ─── Realistic mixed expressions ────────────────────────────────

TEST_CASE("sv_pp_expr: realistic — defined(FOO) && (WIDTH == 32 || WIDTH == 64)") {
    auto g = load_grammar();
    auto ast = parse(g, "defined(FOO) && (WIDTH == 32 || WIDTH == 64)");
    REQUIRE(str_field(ast, "op") == "&&");
    REQUIRE(args_size(ast) == 2);
    auto lhs = arg_at(ast, 0);
    CHECK(str_field(lhs, "type") == "call");
    CHECK(str_field(lhs, "name") == "defined");
    auto rhs = arg_at(ast, 1);
    CHECK(str_field(rhs, "type") == "paren");
    auto rinner = value_field(rhs, "inner");
    REQUIRE(rinner);
    CHECK(str_field(rinner, "op") == "||");
}

TEST_CASE("sv_pp_expr: realistic — !defined(LEGACY)") {
    auto g = load_grammar();
    auto ast = parse(g, "!defined(LEGACY)");
    REQUIRE(str_field(ast, "op") == "!");
    REQUIRE(args_size(ast) == 1);
    auto inner = arg_at(ast, 0);
    CHECK(str_field(inner, "type") == "call");
    CHECK(str_field(inner, "name") == "defined");
}
