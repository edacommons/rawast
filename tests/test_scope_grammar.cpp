// `scope { INNERS }` — string-aware byte scanner. Scope has no
// delimiters of its own; the surrounding sequence's siblings carry
// the start and stop literals. Captured body bytes between the
// previous sibling and the next sibling Key (exclusive) emit as a
// StringValue (default) or ArrayValue of segments (`scope array`).

#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/value.hpp>

#include <sstream>

using namespace rawast;

namespace {

Grammar load(const char* src) {
    register_std_parser_group();
    Grammar g;
    auto r = load_rawast_grammar_from_string(g, src);
    REQUIRE_MESSAGE(r, "load failed: " << (r ? "" : r.error()));
    return g;
}

ValuePtr parse_input(Grammar& g, const std::string& input) {
    std::istringstream is{input};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE_MESSAGE(r, "parse failed for '" << input << "': "
                       << (r ? "" : r.error().message));
    return *r;
}

std::string save_value(Grammar& g, ValuePtr v) {
    std::ostringstream out;
    auto r = g.save(out, std::move(v));
    REQUIRE_MESSAGE(r, "save failed: " << (r ? "" : r.error().message));
    return out.str();
}

} // namespace

// ─── Bare scope (no INNERs): scope captures bytes between siblings ─────

TEST_CASE("scope: bare paren scope captures body bytes") {
    auto g = load(R"GRAM(
        use: std
        start: <PAREN>
        PAREN: sequence { "(", scope { }, ")" }
    )GRAM");
    auto v = parse_input(g, "(hello world)");
    auto sv = as_string(v);
    REQUIRE(sv);
    CHECK(sv->data() == "hello world");
    CHECK(save_value(g, v) == "(hello world)");
}

TEST_CASE("scope: empty body round-trips") {
    auto g = load(R"GRAM(
        use: std
        start: <PAREN>
        PAREN: sequence { "(", scope { }, ")" }
    )GRAM");
    auto v = parse_input(g, "()");
    auto sv = as_string(v);
    REQUIRE(sv);
    CHECK(sv->data() == "");
    CHECK(save_value(g, v) == "()");
}

// ─── String-aware: embedded ')' inside a string doesn't end the scope ────

TEST_CASE("scope: embedded string with ')' inside is captured atomically") {
    auto g = load(R"GRAM(
        use: std
        start: <PAREN>
        PAREN: sequence { "(", scope { std.string }, ")" }
    )GRAM");
    // Input: ( "wait)here" trailing )
    // Without the std.string INNER hint, the naive scanner would
    // close the scope at the ')' inside the quotes.
    auto v = parse_input(g, "(\"wait)here\" trailing)");
    auto sv = as_string(v);
    REQUIRE(sv);
    CHECK(sv->data() == "\"wait)here\" trailing");
    CHECK(save_value(g, v) == "(\"wait)here\" trailing)");
}

// ─── Recursive nesting via self-Ref INNER ──────────────────────────────

TEST_CASE("scope: nested () via self-Ref INNER preserves both layers") {
    auto g = load(R"GRAM(
        use: std
        start: <PAREN>
        PAREN: sequence { "(", scope { <PAREN>, std.string }, ")" }
    )GRAM");
    auto v = parse_input(g, "(outer (inner) tail)");
    auto sv = as_string(v);
    REQUIRE(sv);
    CHECK(sv->data() == "outer (inner) tail");
    CHECK(save_value(g, v) == "(outer (inner) tail)");
}

TEST_CASE("scope: nested scope containing a string with ')'") {
    auto g = load(R"GRAM(
        use: std
        start: <PAREN>
        PAREN: sequence { "(", scope { <PAREN>, std.string }, ")" }
    )GRAM");
    auto v = parse_input(g, "(outer (\"in)ner\") done)");
    auto sv = as_string(v);
    REQUIRE(sv);
    CHECK(sv->data() == "outer (\"in)ner\") done");
    CHECK(save_value(g, v) == "(outer (\"in)ner\") done)");
}

// ─── Scope INNER subparse must inherit caller's ignore_stack ────────────

// When `scope` dispatches a non-leaf INNER rule, the INNER's subparse
// (walk_scan: parse_from) must carry the caller's active ignore
// policy. Without this, an INNER rule with no explicit `ignore`
// declaration loses its inherited ignore set — predictive checks at
// optional boundaries see raw whitespace instead of the eaten policy.
//
// Setup: PROG declares `ignore linespace`. Its body is a scope-array
// dispatching ITEM. ITEM has no explicit ignore — should inherit
// linespace. Between ITEM's mark `a` and the optional `?<TAG>` there
// is a space byte; with proper inheritance the optional's first-byte
// peek runs the ignore first, sees `[`, and pushes TAG. With the bug
// the ignore is empty (fresh subparse stack), peek sees ` `, skips
// the optional → tag never captured.
TEST_CASE("scope: INNER subparse inherits caller's ignore_stack") {
    auto g = load(R"GRAM(
        use: std
        start: <PROG>
        PROG ignore linespace: sequence {
          "{", scope array { <ITEM> }, "}"
        }
        ITEM: sequence dict {
          "a":mark="a",
          ?<TAG>:tag=@
        }
        TAG: sequence dict {
          "[":t="bracket",
          "]"
        }
    )GRAM");
    auto v = parse_input(g, "{a []}");
    auto arr = as_array(v);
    REQUIRE(arr);
    // Layout (with fix): one ITEM segment whose dict has tag=bracket.
    // (with bug): ITEM segment with no tag, plus separate text/raw bytes.
    REQUIRE(arr->data().size() >= 1);
    auto item = std::dynamic_pointer_cast<DictValue>(arr->data()[0]);
    REQUIRE_MESSAGE(item, "first segment isn't ITEM dict");
    auto tag_it = item->data().find("tag");
    REQUIRE_MESSAGE(tag_it != item->data().end(),
                    "ITEM's optional ?<TAG> didn't capture — inherited "
                    "linespace was lost at scope/INNER subparse boundary");
    auto tag_d = std::dynamic_pointer_cast<DictValue>(tag_it->second);
    REQUIRE(tag_d);
    auto t_sv = as_string(tag_d->data()["t"]);
    REQUIRE(t_sv);
    CHECK(t_sv->data() == "bracket");
}

// ─── Different bracket flavours ─────────────────────────────────────────

TEST_CASE("scope: square brackets work the same as parens") {
    auto g = load(R"GRAM(
        use: std
        start: <SQ>
        SQ: sequence { "[", scope { std.string }, "]" }
    )GRAM");
    auto v = parse_input(g, "[a \"with]bracket\" b]");
    auto sv = as_string(v);
    REQUIRE(sv);
    CHECK(sv->data() == "a \"with]bracket\" b");
    CHECK(save_value(g, v) == "[a \"with]bracket\" b]");
}

// ─── Array container — segment-list output ──────────────────────────────

TEST_CASE("scope array: empty body emits empty array") {
    auto g = load(R"GRAM(
        use: std
        start: <DEMO>
        DEMO: sequence { "(", scope array { std.int }, ")" }
    )GRAM");
    auto v = parse_input(g, "()");
    auto arr = as_array(v);
    REQUIRE(arr);
    CHECK(arr->data().empty());
    CHECK(save_value(g, v) == "()");
}

TEST_CASE("scope array: pure text body becomes one StringValue segment") {
    auto g = load(R"GRAM(
        use: std
        start: <DEMO>
        DEMO: sequence { "(", scope array { std.int }, ")" }
    )GRAM");
    auto v = parse_input(g, "(hello world)");
    auto arr = as_array(v);
    REQUIRE(arr);
    REQUIRE(arr->data().size() == 1);
    auto sv = as_string(arr->data()[0]);
    REQUIRE(sv);
    CHECK(sv->data() == "hello world");
    CHECK(save_value(g, v) == "(hello world)");
}

TEST_CASE("scope array: text + INNER + text → mixed segments in order") {
    auto g = load(R"GRAM(
        use: std
        start: <DEMO>
        DEMO: sequence { "(", scope array { std.int }, ")" }
    )GRAM");
    auto v = parse_input(g, "(a 42 b)");
    auto arr = as_array(v);
    REQUIRE(arr);
    REQUIRE(arr->data().size() == 3);
    auto s0 = as_string(arr->data()[0]); REQUIRE(s0); CHECK(s0->data() == "a ");
    auto i1 = std::dynamic_pointer_cast<IntValue>(arr->data()[1]);
    REQUIRE(i1); CHECK(i1->data() == 42);
    auto s2 = as_string(arr->data()[2]); REQUIRE(s2); CHECK(s2->data() == " b");
    CHECK(save_value(g, v) == "(a 42 b)");
}

TEST_CASE("scope array: multiple INNER matches preserve order") {
    auto g = load(R"GRAM(
        use: std
        start: <DEMO>
        DEMO: sequence { "(", scope array { std.int }, ")" }
    )GRAM");
    auto v = parse_input(g, "(1 2 3)");
    auto arr = as_array(v);
    REQUIRE(arr);
    // Layout: 1, " ", 2, " ", 3 — five segments.
    REQUIRE(arr->data().size() == 5);
    CHECK(std::dynamic_pointer_cast<IntValue>(arr->data()[0])->data() == 1);
    CHECK(as_string(arr->data()[1])->data() == " ");
    CHECK(std::dynamic_pointer_cast<IntValue>(arr->data()[2])->data() == 2);
    CHECK(as_string(arr->data()[3])->data() == " ");
    CHECK(std::dynamic_pointer_cast<IntValue>(arr->data()[4])->data() == 3);
    CHECK(save_value(g, v) == "(1 2 3)");
}

// ─── Ref-to-rule INNERs ─────────────────────────────────────────────────

TEST_CASE("scope array: Ref-to-rule INNER produces typed dict segment") {
    auto g = load(R"GRAM(
        use: std
        start: <DEMO>
        DEMO: sequence { "(", scope array { <REF> }, ")" }
        REF: sequence dict { identifier:type="ref":value=@ }
    )GRAM");
    auto v = parse_input(g, "(FOO BAR)");
    auto arr = as_array(v);
    REQUIRE(arr);
    // Layout: {ref:FOO}, " ", {ref:BAR}
    REQUIRE(arr->data().size() == 3);
    auto seg0 = std::dynamic_pointer_cast<DictValue>(arr->data()[0]);
    REQUIRE(seg0);
    auto type0 = std::dynamic_pointer_cast<StringValue>(seg0->data()["type"]);
    auto value0 = std::dynamic_pointer_cast<StringValue>(seg0->data()["value"]);
    REQUIRE(type0); CHECK(type0->data() == "ref");
    REQUIRE(value0); CHECK(value0->data() == "FOO");
    CHECK(as_string(arr->data()[1])->data() == " ");
    auto seg2 = std::dynamic_pointer_cast<DictValue>(arr->data()[2]);
    REQUIRE(seg2);
    auto value2 = std::dynamic_pointer_cast<StringValue>(seg2->data()["value"]);
    REQUIRE(value2); CHECK(value2->data() == "BAR");
    CHECK(save_value(g, v) == "(FOO BAR)");
}

// ─── Multi-stop scope (inside repeat + separator) ──────────────────────────

// A scope placed inside `repeat ... separator X` followed by a closing
// sibling Y can naturally appear in two terminal contexts:
//
//   - Non-final element: terminates at the separator X.
//   - Final element:     terminates at the post-repeat sibling Y.
//
// The byte-scan must accept BOTH literals as stops. This is the pattern
// macro arg lists need: `(arg1, arg2, arg3)` — each ARG body is a
// balanced-token run ending at `,` or `)`. The engine resolves the
// scope's stop set at load time by walking down into the surrounding
// repeat to pick up both the separator and the post-repeat sibling.
TEST_CASE("scope: stops include both repeat separator and post-repeat sibling") {
    auto g = load(R"GRAM(
        use: std
        start: <PROG>
        PROG: sequence array {
          "(",
          repeat scope { std.string } separator ",",
          ")"
        }
    )GRAM");
    auto v = parse_input(g, "(alpha,beta,gamma)");
    auto arr = as_array(v);
    REQUIRE(arr);
    REQUIRE(arr->data().size() == 3);
    CHECK(as_string(arr->data()[0])->data() == "alpha");
    CHECK(as_string(arr->data()[1])->data() == "beta");
    CHECK(as_string(arr->data()[2])->data() == "gamma");
}
