// `#opchain` reserved-binding annotation. Mark a rule whose parse
// output has the always-wrap `{lhs, tail:[{op, rhs}, ...]}` shape; the
// engine post-processes the subtree to the compacted `{op, args[]}`
// shape (left-fold semantics: same-op runs collapse flat, mixed-op
// boundaries nest). Save reverses the transform.
//
// AST contract after compaction:
//   * Atom (no op/args): unchanged.
//   * `{op: O, args: [a, b]}` (binary): left-fold means a is the left
//     operand, b is the right.
//   * `{op: O, args: [a, b, c, ...]}` (n-ary, args.size > 2): left-
//     fold means `((a O b) O c) O ...`. Always same-op chain.
//   * Mixed-op chains nest at op boundaries — every node still has
//     `{op, args[]}` shape but the args may themselves be `{op, args[]}`.

#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/value.hpp>

#include <sstream>

using namespace rawast;

namespace {

const char* OPCHAIN_GRAMMAR = R"(
use: std
start: <EXPR>

EXPR ignore whitespace: <ADD>:#opchain

ADD: sequence dict {
    <NUM>:lhs=@,
    repeat <ADD_TAIL>:tail[]=@
}
ADD_TAIL: sequence dict {
    choice { '+':op="+", '-':op="-" },
    <NUM>:rhs=@
}

NUM: sequence dict {
    int:type="num":value=@
}
)";

Grammar load_opchain_grammar() {
    register_std_parser_group();
    Grammar g;
    auto r = load_rawast_grammar_from_string(g, OPCHAIN_GRAMMAR);
    REQUIRE_MESSAGE(r, "load failed: " << (r ? "" : r.error()));
    return g;
}

ValuePtr parse(Grammar& g, const std::string& input) {
    std::istringstream is{input};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE_MESSAGE(r, "parse failed for '" << input << "': "
                       << (r ? "" : r.error().message));
    return *r;
}

std::string save(Grammar& g, ValuePtr v) {
    std::ostringstream out;
    auto r = g.save(out, std::move(v));
    REQUIRE_MESSAGE(r, "save failed: " << (r ? "" : r.error().message));
    return out.str();
}

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

// ─── Phase 1: meta-grammar + loader accept bare `:#opchain` ──────

TEST_CASE("opchain: meta-grammar accepts bare `:#opchain` (no value)") {
    Grammar g;
    register_std_parser_group();
    auto r = load_rawast_grammar_from_string(g, OPCHAIN_GRAMMAR);
    CHECK_MESSAGE(r, "loader should accept :#opchain bare flag: "
                     << (r ? "" : r.error()));
}

TEST_CASE("opchain: loader rejects `:#opchain=VALUE` (flag takes no value)") {
    Grammar g;
    register_std_parser_group();
    const char* bad = R"(
        use: std
        start: <X>
        X: <Y>:#opchain="oops"
        Y: identifier
    )";
    auto r = load_rawast_grammar_from_string(g, bad);
    REQUIRE_FALSE(r);
    CHECK(r.error().find("opchain") != std::string::npos);
}

// ─── Phase 2: parse-side compaction ───────────────────────────────

TEST_CASE("opchain parse: atom passes through unchanged") {
    auto g = load_opchain_grammar();
    auto ast = parse(g, "1");
    CHECK(str_field(ast, "type") == "num");
}

TEST_CASE("opchain parse: same-op chain collapses to flat n-ary `{op, args[]}`") {
    auto g = load_opchain_grammar();
    auto ast = parse(g, "1+2+3");
    CHECK(str_field(ast, "op") == "+");
    REQUIRE(args_size(ast) == 3);
    // No `lhs` / `tail` left after compaction.
    CHECK_FALSE(value_field(ast, "lhs"));
    CHECK_FALSE(value_field(ast, "tail"));
}

TEST_CASE("opchain parse: mixed-op chain nests at op boundaries") {
    auto g = load_opchain_grammar();
    // `1+2-3` → `{op:"-", args:[{op:"+", args:[1,2]}, 3]}`
    auto ast = parse(g, "1+2-3");
    CHECK(str_field(ast, "op") == "-");
    REQUIRE(args_size(ast) == 2);
    auto inner = arg_at(ast, 0);
    CHECK(str_field(inner, "op") == "+");
    REQUIRE(args_size(inner) == 2);
}

TEST_CASE("opchain parse: single binop is `{op, args:[a, b]}` (binary)") {
    auto g = load_opchain_grammar();
    auto ast = parse(g, "1-2");
    CHECK(str_field(ast, "op") == "-");
    REQUIRE(args_size(ast) == 2);
}

// ─── Phase 3: save-side expansion ────────────────────────────────

TEST_CASE("opchain save: round-trip atom") {
    auto g = load_opchain_grammar();
    auto ast = parse(g, "1");
    CHECK(save(g, ast) == "1");
}

TEST_CASE("opchain save: round-trip same-op chain") {
    auto g = load_opchain_grammar();
    auto ast = parse(g, "1+2+3");
    CHECK(save(g, ast) == "1+2+3");
}

TEST_CASE("opchain save: round-trip mixed-op chain") {
    auto g = load_opchain_grammar();
    auto ast = parse(g, "1+2-3");
    CHECK(save(g, ast) == "1+2-3");
}

TEST_CASE("opchain save: round-trip alternating ops") {
    auto g = load_opchain_grammar();
    auto ast = parse(g, "1-2+3-4");
    CHECK(save(g, ast) == "1-2+3-4");
}
