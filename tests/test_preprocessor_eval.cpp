// Unit tests for the generic preprocessor expression evaluator
// (default_pp_expr_eval). Synthesized ASTs only — no grammar
// dependency — so we can exercise every AST shape and the
// ref_resolver contract in isolation.
//
// Integration tests that parse real `\`if conditions through the SV
// COND_EXPR and feed the resulting AST through the same evaluator live
// in tests/test_sv_preprocessor.cpp (the "built-in eval" cases).

#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/preprocessor.hpp>
#include <rawast/value.hpp>

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

using namespace rawast;

namespace {

// Tri-state predicates over the ValuePtr result of default_pp_expr_eval.
bool is_true(const ValuePtr& v) {
    return v && v->type() == ValueType::Bool &&
           static_cast<const BoolValue*>(v.get())->data();
}
bool is_false(const ValuePtr& v) {
    return v && v->type() == ValueType::Bool &&
           !static_cast<const BoolValue*>(v.get())->data();
}
bool is_undef(const ValuePtr& v) { return v && v->type() == ValueType::Undefined; }

// ─── Synthesized-AST builders ──────────────────────────────────────
// Same conventions as in test_preprocessor.cpp — kept local here so
// this file stands alone.

ValuePtr str(std::string s) { return make_string(std::move(s)); }
ValuePtr i(std::int64_t v)  { return make_int(v); }

ValuePtr arr(std::initializer_list<ValuePtr> items) {
    auto a = std::make_shared<ArrayValue>();
    for (auto& it : items) a->data().push_back(it);
    return a;
}

ValuePtr dict(std::initializer_list<std::pair<std::string, ValuePtr>> entries) {
    auto d = std::make_shared<DictValue>();
    for (auto& [k, v] : entries) d->data().emplace(k, v);
    return d;
}

// Leaf shorthands matching the grammar's AST contract.
ValuePtr int_(std::int64_t v) { return dict({{"type", str("int")}, {"value", i(v)}}); }
ValuePtr ref_(const std::string& name) {
    return dict({{"type", str("ref")}, {"value", str(name)}});
}
ValuePtr paren_(ValuePtr inner) {
    return dict({{"type", str("paren")}, {"value", inner}});
}
ValuePtr call_(const std::string& name, std::initializer_list<ValuePtr> args) {
    return dict({{"type", str("call")},
                 {"name", str(name)},
                 {"args", arr(args)}});
}
ValuePtr defined_(const std::string& name) {
    return call_("defined", {ref_(name)});
}
ValuePtr op_(const std::string& op, std::initializer_list<ValuePtr> args) {
    return dict({{"op", str(op)}, {"args", arr(args)}});
}

// Stub resolvers.
auto resolver_empty() {
    return [](const std::string&) -> std::optional<std::string> {
        return std::nullopt;
    };
}
auto resolver_const(std::initializer_list<std::pair<std::string, std::string>> entries) {
    std::unordered_map<std::string, std::string> table;
    for (auto& [k, v] : entries) table.emplace(k, v);
    return [table](const std::string& name) -> std::optional<std::string> {
        auto it = table.find(name);
        if (it == table.end()) return std::nullopt;
        return it->second;
    };
}

} // namespace

// ─── Leaves ────────────────────────────────────────────────────────

TEST_CASE("default_pp_expr_eval: int(0) → false, int(non-zero) → true") {
    auto r = resolver_empty();
    CHECK(is_false(default_pp_expr_eval(int_(0), r)));
    CHECK(is_true(default_pp_expr_eval(int_(1), r)));
    CHECK(is_true(default_pp_expr_eval(int_(-5), r)));
}

TEST_CASE("default_pp_expr_eval: ref(undefined) → false") {
    auto r = resolver_empty();
    CHECK(is_false(default_pp_expr_eval(ref_("FOO"), r)));
}

TEST_CASE("default_pp_expr_eval: ref(defined, body=\"0\") → false") {
    auto r = resolver_const({{"FOO", "0"}});
    CHECK(is_false(default_pp_expr_eval(ref_("FOO"), r)));
}

TEST_CASE("default_pp_expr_eval: ref(defined, body=\"1\") → true") {
    auto r = resolver_const({{"FOO", "1"}});
    CHECK(is_true(default_pp_expr_eval(ref_("FOO"), r)));
}

TEST_CASE("default_pp_expr_eval: ref(defined, non-int body) → true (defined-is-truthy)") {
    auto r = resolver_const({{"FOO", "abc"}});
    CHECK(is_true(default_pp_expr_eval(ref_("FOO"), r)));
}

TEST_CASE("default_pp_expr_eval: paren passes through") {
    auto r = resolver_empty();
    CHECK(is_true(default_pp_expr_eval(paren_(int_(7)), r)));
    CHECK(is_false(default_pp_expr_eval(paren_(int_(0)), r)));
}

// ─── defined() ─────────────────────────────────────────────────────

TEST_CASE("default_pp_expr_eval: defined(FOO) reflects resolver state") {
    auto r1 = resolver_empty();
    CHECK(is_false(default_pp_expr_eval(defined_("FOO"), r1)));

    auto r2 = resolver_const({{"FOO", ""}});
    CHECK(is_true(default_pp_expr_eval(defined_("FOO"), r2)));
}

TEST_CASE("default_pp_expr_eval: unknown call name → nullopt") {
    auto r = resolver_empty();
    auto v = call_("custom_pred", {int_(1)});
    CHECK(is_undef(default_pp_expr_eval(v, r)));
}

// ─── Operators: boolean ────────────────────────────────────────────

TEST_CASE("default_pp_expr_eval: ! flips truthiness") {
    auto r = resolver_empty();
    CHECK(is_true(default_pp_expr_eval(op_("!", {int_(0)}), r)));
    CHECK(is_false(default_pp_expr_eval(op_("!", {int_(1)}), r)));
}

TEST_CASE("default_pp_expr_eval: || short-circuits on first true") {
    auto r = resolver_empty();
    // First arg true short-circuits — second arg unevaluated, even if
    // undecidable. We can't observe lazy-vs-eager directly, but the
    // overall result must be `true` regardless.
    auto v = op_("||", {int_(1), call_("unknown", {})});
    CHECK(is_true(default_pp_expr_eval(v, r)));
}

TEST_CASE("default_pp_expr_eval: || returns nullopt when no true + any undecidable") {
    auto r = resolver_empty();
    auto v = op_("||", {int_(0), call_("unknown", {})});
    CHECK(is_undef(default_pp_expr_eval(v, r)));
}

TEST_CASE("default_pp_expr_eval: && short-circuits on first false") {
    auto r = resolver_empty();
    auto v = op_("&&", {int_(0), call_("unknown", {})});
    CHECK(is_false(default_pp_expr_eval(v, r)));
}

TEST_CASE("default_pp_expr_eval: && returns nullopt when no false + any undecidable") {
    auto r = resolver_empty();
    auto v = op_("&&", {int_(1), call_("unknown", {})});
    CHECK(is_undef(default_pp_expr_eval(v, r)));
}

TEST_CASE("default_pp_expr_eval: defined(FOO) && (WIDTH == 32 || WIDTH == 64)") {
    auto r = resolver_const({{"FOO", "1"}, {"WIDTH", "32"}});
    auto v = op_("&&", {
        defined_("FOO"),
        paren_(op_("||", {
            op_("==", {ref_("WIDTH"), int_(32)}),
            op_("==", {ref_("WIDTH"), int_(64)})
        }))
    });
    CHECK(is_true(default_pp_expr_eval(v, r)));
}

// ─── Operators: comparisons ────────────────────────────────────────

TEST_CASE("default_pp_expr_eval: == compares ints") {
    auto r = resolver_const({{"WIDTH", "32"}});
    CHECK(is_true(default_pp_expr_eval(op_("==", {ref_("WIDTH"), int_(32)}), r)));
    CHECK(is_false(default_pp_expr_eval(op_("==", {ref_("WIDTH"), int_(64)}), r)));
}

TEST_CASE("default_pp_expr_eval: !=, <, >, <=, >=") {
    auto r = resolver_empty();
    CHECK(is_true(default_pp_expr_eval(op_("!=", {int_(1), int_(2)}), r)));
    CHECK(is_true(default_pp_expr_eval(op_("<",  {int_(1), int_(2)}), r)));
    CHECK(is_false(default_pp_expr_eval(op_(">",  {int_(1), int_(2)}), r)));
    CHECK(is_true(default_pp_expr_eval(op_("<=", {int_(2), int_(2)}), r)));
    CHECK(is_true(default_pp_expr_eval(op_(">=", {int_(2), int_(2)}), r)));
}

TEST_CASE("default_pp_expr_eval: undefined ref in comparison → nullopt") {
    auto r = resolver_empty();
    auto v = op_("==", {ref_("UNKNOWN"), int_(0)});
    CHECK(is_undef(default_pp_expr_eval(v, r)));
}

// ─── Operators: arithmetic ─────────────────────────────────────────

TEST_CASE("default_pp_expr_eval: arithmetic feeds comparison: A + 1 == 33") {
    auto r = resolver_const({{"A", "32"}});
    auto v = op_("==", {
        op_("+", {ref_("A"), int_(1)}),
        int_(33)
    });
    CHECK(is_true(default_pp_expr_eval(v, r)));
}

TEST_CASE("default_pp_expr_eval: division by zero → nullopt") {
    auto r = resolver_empty();
    auto v = op_("/", {int_(10), int_(0)});
    CHECK(is_undef(default_pp_expr_eval(v, r)));
}

// Integration tests that exercise the built-in eval end-to-end
// (parse → AST → walker → evaluator → branch selection) live in
// tests/test_sv_preprocessor.cpp, which composes the real merged
// grammar instead of synthesizing a walker-compatible AST by hand.

