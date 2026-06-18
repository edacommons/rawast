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

// ─── Scope INNER subparse inheritance is symmetric across all run_ignore sites ──

// The predictive-only inheritance fix (12553ba) seeded the caller's
// ignore into should_skip_optional but left run_ignore at Key/Parse/
// Sequence sites stuck with the empty stack. So an INNER rule with
// required inter-item whitespace tolerance (no optional boundary
// between the tokens) parses standalone but fails when dispatched as
// a scope INNER — semantic asymmetry between scope-INNER and normal
// Ref dispatch.
//
// This test pins the symmetry. INNER has three required items
// (`begin` identifier `end`) separated by spaces. The grammar's
// `ignore linespace` policy must reach all three run_ignore sites
// inside INNER, not just predictive ones.
TEST_CASE("scope: INNER's inter-item whitespace works under inherited ignore") {
    auto g = load(R"GRAM(
        use: std
        start: <PROG>
        PROG ignore linespace: sequence array {
          "(", scope array { <INNER> }, ")"
        }
        INNER: sequence dict {
          "begin":t="begin",
          identifier:name=@,
          "end"
        }
    )GRAM");
    auto v = parse_input(g, "(begin foo end)");
    auto outer = as_array(v);
    REQUIRE(outer);
    // PROG's sequence-array carries one element — the scope's array.
    REQUIRE(outer->data().size() == 1);
    auto segs = std::dynamic_pointer_cast<ArrayValue>(outer->data()[0]);
    REQUIRE(segs);
    // Scope's segments must contain exactly one INNER dict — INNER
    // matched the whole span as one atomic structured value. With
    // the asymmetry, INNER fails on the first inter-item space and
    // walk_scan eats bytes raw, producing a StringValue text-run
    // segment instead.
    REQUIRE(segs->data().size() == 1);
    auto inner = std::dynamic_pointer_cast<DictValue>(segs->data()[0]);
    REQUIRE_MESSAGE(inner,
        "INNER didn't produce a structured segment — its inter-item "
        "run_ignore couldn't see the inherited linespace policy");
    auto name = as_string(inner->data()["name"]);
    REQUIRE(name);
    CHECK(name->data() == "foo");
}

// ─── Choice-first INNER under leading whitespace ────────────────────────

// Issue #6: when a scope-dispatched INNER's first item is a Choice and
// the cursor is at whitespace (with the surrounding grammar's
// ignore policy seeded into the subparse), the first-content guard
// suppresses run_ignore before choice_alt_cant_match. The guard
// raises the question: does this correctly preserve scope byte
// boundaries, or does it cause silent misparse where alts get
// wrongly skipped?
//
// Expected: walk_scan rejects the INNER at the whitespace position
// (all alts skipped because none of their first-byte sets include
// whitespace), eats the space as raw bytes into text_run, then
// dispatches INNER again at the content position — where Choice
// alts DO match. Final layout: leading whitespace as text segment,
// INNER's structured value as next segment.
TEST_CASE("scope: Choice-first INNER under leading whitespace dispatches correctly") {
    auto g = load(R"GRAM(
        use: std
        start: <PROG>
        PROG ignore linespace: sequence array {
          "(",
          scope array { <ITEM> },
          ")"
        }
        ITEM: choice {
          <NUM>,
          <IDENT>
        }
        NUM: sequence dict { std.int:type="num":value=@ }
        IDENT: sequence dict { identifier:type="ident":value=@ }
    )GRAM");
    auto v = parse_input(g, "( foo )");
    auto outer = as_array(v);
    REQUIRE(outer);
    REQUIRE(outer->data().size() == 1);
    auto segs = std::dynamic_pointer_cast<ArrayValue>(outer->data()[0]);
    REQUIRE(segs);
    // Expected layout: leading " " text, IDENT segment, trailing " "
    // text. The Choice-first INNER must match the identifier `foo`
    // at the post-whitespace position, with the leading space
    // captured as a scope text-run, not absorbed into the INNER.
    bool found_ident = false;
    for (const auto& seg : segs->data()) {
        if (auto d = std::dynamic_pointer_cast<DictValue>(seg)) {
            auto t = as_string(d->data()["type"]);
            if (t && t->data() == "ident") {
                auto val = as_string(d->data()["value"]);
                REQUIRE(val);
                CHECK(val->data() == "foo");
                found_ident = true;
            }
        }
    }
    REQUIRE_MESSAGE(found_ident,
        "Choice-first INNER didn't match — leading-whitespace guard may "
        "have permanently skipped alts instead of letting walk_scan "
        "retry at the content position");
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
