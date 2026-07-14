// Tests for the SV preprocessor grammar's `\`define` rule + segmented
// body. Verifies parse → AST shape and parse → save round-trip.
//
// `"\n"` in the grammar source unescapes to a real newline byte
// thanks to the loader's Key-literal unescape pass — see
// `unescape_key_literal` in src/loader.cpp.

#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_sv.hpp>
#include <rawast/preprocessor.hpp>
#include <rawast/value.hpp>

#include <sstream>

using namespace rawast;

namespace {

Grammar load_grammar() {
    register_std_parser_group();
    register_sv_parser_group();
    Grammar g;
    auto r = load_rawast_grammar_from_file(g, "grammars/systemverilog.rawast");
    REQUIRE_MESSAGE(r, "loading systemverilog.rawast failed: "
                       << (r ? "" : r.error()));
    return g;
}

// Parse a SINGLE preprocessor construct (one directive / macro use) via
// the PP_CONSTRUCT rule and return its AST dict. This is what the scan
// driver applies at each backtick; there's no whole-file PP_FILE rule
// anymore. Inputs here are one construct (its own trailing `\n` where the
// rule consumes it).
ValuePtr parse(Grammar& g, const std::string& src) {
    auto stream = Stream::from_string(src);
    ValuePool pool;
    // require_full_consume=false: the scan driver skips a directive's
    // trailing newline itself, so the flat rules (undef/ifdef/…) don't
    // consume it — a standalone parse leaves it behind.
    auto r = g.parse_from(stream, pool, g.rule_id("PP_CONSTRUCT"),
                          /*require_full_consume=*/false);
    REQUIRE_MESSAGE(r, "parse failed for '" << src << "': "
                       << (r ? "" : r.error().message));
    return *r;
}

std::string save(Grammar& g, ValuePtr v) {
    // Save one construct back through PP_CONSTRUCT (the choice dispatches
    // on the dict's `type` discriminator).
    std::ostringstream out;
    auto r = g.save(out, std::move(v), true, g.rule_id("PP_CONSTRUCT"));
    REQUIRE_MESSAGE(r, "save failed: " << (r ? "" : r.error().message));
    return out.str();
}

std::string str_field(const ValuePtr& v, const std::string& key) {
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    if (!d) return {};
    auto it = d->data().find(key);
    if (it == d->data().end()) return {};
    auto s = as_string(it->second);
    return s ? s->data() : std::string{};
}

// sv_preprocessor.rawast nests name + params under a `decl` sub-dict.
// These helpers transparently follow the indirection.
std::string name_of(const ValuePtr& ast) {
    auto d = std::dynamic_pointer_cast<DictValue>(ast);
    if (!d) return {};
    if (auto it = d->data().find("decl"); it != d->data().end()) {
        return str_field(it->second, "name");
    }
    return str_field(ast, "name");
}

std::shared_ptr<ArrayValue> params_of(const ValuePtr& ast) {
    auto d = std::dynamic_pointer_cast<DictValue>(ast);
    if (!d) return {};
    if (auto it = d->data().find("decl"); it != d->data().end()) {
        if (auto dd = std::dynamic_pointer_cast<DictValue>(it->second)) {
            auto p = dd->data().find("params");
            if (p != dd->data().end()) {
                return std::dynamic_pointer_cast<ArrayValue>(p->second);
            }
        }
        return nullptr;
    }
    auto p = d->data().find("params");
    if (p == d->data().end()) return nullptr;
    return std::dynamic_pointer_cast<ArrayValue>(p->second);
}

bool has_params(const ValuePtr& ast) {
    auto d = std::dynamic_pointer_cast<DictValue>(ast);
    if (!d) return false;
    if (auto it = d->data().find("decl"); it != d->data().end()) {
        if (auto dd = std::dynamic_pointer_cast<DictValue>(it->second)) {
            return dd->data().find("params") != dd->data().end();
        }
        return false;
    }
    return d->data().find("params") != d->data().end();
}

// Trim leading/trailing horizontal whitespace. `*:cond=@` (Raw) in
// the grammar bypasses the ignore-set, so cond strings include the
// space the keyword separator left behind. The walker trims before
// expr_eval; shape tests use this helper.
std::string trim_ws(std::string s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    std::size_t j = s.size();
    while (j > i && (s[j-1] == ' ' || s[j-1] == '\t')) --j;
    return s.substr(i, j - i);
}

std::shared_ptr<ArrayValue> body_of(const ValuePtr& ast) {
    auto d = std::dynamic_pointer_cast<DictValue>(ast);
    REQUIRE(d);
    auto it = d->data().find("body");
    REQUIRE(it != d->data().end());
    return std::dynamic_pointer_cast<ArrayValue>(it->second);
}

// DEFINE_BODY now captures the body raw (text runs + null LINE_CONT
// markers) and defers segmentation to first expansion, which parses the
// body through the MACRO_BODY rule. These shape tests exercise that same
// segmenter: reconstruct the raw body text and run MACRO_BODY over it
// (with the trailing `\n` sentinel MACRO_BODY's scope stops on), and
// return the resulting `segments` array — the shape the tests assert on.
std::shared_ptr<ArrayValue> body_segments_of(Grammar& g, const ValuePtr& ast) {
    auto raw = body_of(ast);
    REQUIRE(raw);
    // DEFINE_BODY captures text runs + {type:"string"} segments (its
    // <STRING> inner makes strings atomic so a `//` inside one isn't a
    // comment). Reconstruct the raw body text — re-quoting strings.
    std::string text;
    for (auto& seg : raw->data()) {
        if (auto s = as_string(seg)) { text += s->data(); continue; }
        if (auto d = std::dynamic_pointer_cast<DictValue>(seg))
            if (str_field(d, "type") == "string")
                text += "\"" + str_field(d, "value") + "\"";
    }
    auto stream = Stream::from_string(text + "\n");
    ValuePool pool;
    auto r = g.parse_from(stream, pool, g.rule_id("MACRO_BODY"),
                          /*require_full_consume=*/false);
    REQUIRE_MESSAGE(r, "MACRO_BODY parse failed for body '" << text << "'");
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);
    auto it = d->data().find("segments");
    REQUIRE(it != d->data().end());
    return std::dynamic_pointer_cast<ArrayValue>(it->second);
}

// A macro arg is now a SEGMENT-LIST (MACRO_ARG scope array), not a flat
// StringValue. Render it back to its source text for shape assertions.
std::string arg_text(const ValuePtr& seg) {
    if (!seg) return {};
    if (auto arr = std::dynamic_pointer_cast<ArrayValue>(seg)) {
        std::string out;
        for (auto& s : arr->data()) out += arg_text(s);
        return out;
    }
    if (auto s = as_string(seg)) return s->data();
    auto d = std::dynamic_pointer_cast<DictValue>(seg);
    if (!d) return {};
    auto ty = str_field(d, "type");
    if (ty == "ref") return str_field(d, "value");
    if (ty == "string") return "\"" + str_field(d, "value") + "\"";
    if (ty == "group_paren" || ty == "group_brace" || ty == "group_bracket") {
        const char* o = ty == "group_brace" ? "{" : ty == "group_bracket" ? "[" : "(";
        const char* c = ty == "group_brace" ? "}" : ty == "group_bracket" ? "]" : ")";
        std::string r = o;
        auto it = d->data().find("items");
        if (it != d->data().end()) r += arg_text(it->second);
        return r + c;
    }
    if (ty == "macro_use") {
        std::string r = "`" + str_field(d, "name");
        if (auto a = d->data().find("args"); a != d->data().end())
            if (auto arr = std::dynamic_pointer_cast<ArrayValue>(a->second)) {
                r += "(";
                for (std::size_t i = 0; i < arr->data().size(); ++i) {
                    if (i) r += ",";
                    r += arg_text(arr->data()[i]);
                }
                r += ")";
            }
        return r;
    }
    return {};
}

} // namespace

// ─── Top-level shape ──────────────────────────────────────────────

TEST_CASE("sv_pp define: empty body → {type:'define', name, body:[]}") {
    auto g = load_grammar();
    auto ast = parse(g, "`define FOO\n");
    CHECK(str_field(ast, "type") == "define");
    CHECK(name_of(ast) == "FOO");
    auto body = body_segments_of(g, ast);
    REQUIRE(body);
    CHECK(body->data().empty());
}

TEST_CASE("sv_pp define: simple text body → one StringValue segment") {
    auto g = load_grammar();
    auto ast = parse(g, "`define FOO bar\n");
    CHECK(name_of(ast) == "FOO");
    auto body = body_segments_of(g, ast);
    REQUIRE(body);
    // PARAM_REF picks up the identifier `bar` as a typed segment.
    REQUIRE(body->data().size() == 1);
    auto seg = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg);
    CHECK(str_field(seg, "type") == "ref");
    CHECK(str_field(seg, "value") == "bar");
}

TEST_CASE("sv_pp define: param-like identifier followed by text") {
    auto g = load_grammar();
    auto ast = parse(g, "`define FOO x + 1\n");
    auto body = body_segments_of(g, ast);
    REQUIRE(body);
    // Layout: {ref:x}, " + 1"   — text gap after PARAM_REF coalesces
    REQUIRE(body->data().size() == 2);
    auto seg0 = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg0);
    CHECK(str_field(seg0, "type") == "ref");
    CHECK(str_field(seg0, "value") == "x");
    auto seg1 = as_string(body->data()[1]);
    REQUIRE(seg1);
    CHECK(seg1->data() == " + 1");
}

TEST_CASE("sv_pp define: string literal inside body is atomic") {
    auto g = load_grammar();
    auto ast = parse(g, "`define MSG \"hello world\"\n");
    auto body = body_segments_of(g, ast);
    REQUIRE(body);
    REQUIRE(body->data().size() == 1);
    auto seg = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg);
    CHECK(str_field(seg, "type") == "string");
    CHECK(str_field(seg, "value") == "hello world");
}

TEST_CASE("sv_pp define: macro_use inside body emits typed segment") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `B\n");
    auto body = body_segments_of(g, ast);
    REQUIRE(body);
    REQUIRE(body->data().size() == 1);
    auto seg = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg);
    CHECK(str_field(seg, "type") == "macro_use");
    CHECK(str_field(seg, "name") == "B");
}

TEST_CASE("sv_pp define: mixed body — ref + text + string + macro_use") {
    auto g = load_grammar();
    auto ast = parse(g, "`define COMBO x = \"v\" + `OTHER\n");
    auto body = body_segments_of(g, ast);
    REQUIRE(body);
    // Layout: {ref:x}, " = ", {string:"v"}, " + ", {macro_use:OTHER}
    REQUIRE(body->data().size() == 5);
    auto s0 = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(s0); CHECK(str_field(s0, "type") == "ref");
    CHECK(str_field(s0, "value") == "x");
    auto s1 = as_string(body->data()[1]);
    REQUIRE(s1); CHECK(s1->data() == " = ");
    auto s2 = std::dynamic_pointer_cast<DictValue>(body->data()[2]);
    REQUIRE(s2); CHECK(str_field(s2, "type") == "string");
    CHECK(str_field(s2, "value") == "v");
    auto s3 = as_string(body->data()[3]);
    REQUIRE(s3); CHECK(s3->data() == " + ");
    auto s4 = std::dynamic_pointer_cast<DictValue>(body->data()[4]);
    REQUIRE(s4); CHECK(str_field(s4, "type") == "macro_use");
    CHECK(str_field(s4, "name") == "OTHER");
}

// ─── Round-trip ────────────────────────────────────────────────────

TEST_CASE("sv_pp define: empty body round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define FOO\n");
    // Canonical save emits the identifier's `space`, trimmed from the
    // line end by the pretty trailing-whitespace pass — semantically
    // equivalent to the parse input (empty body).
    CHECK(save(g, ast) == "`define FOO\n");
}

TEST_CASE("sv_pp define: simple body round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define FOO bar\n");
    CHECK(save(g, ast) == "`define FOO bar\n");
}

TEST_CASE("sv_pp define: string body round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define MSG \"hello world\"\n");
    CHECK(save(g, ast) == "`define MSG \"hello world\"\n");
}

TEST_CASE("sv_pp define: macro_use body round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `B\n");
    CHECK(save(g, ast) == "`define A `B\n");
}

TEST_CASE("sv_pp define: mixed body round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define COMBO x = \"v\" + `OTHER\n");
    CHECK(save(g, ast) == "`define COMBO x = \"v\" + `OTHER\n");
}

// ─── Macro parameters ─────────────────────────────────────────────

TEST_CASE("sv_pp define: macro with one parameter") {
    auto g = load_grammar();
    auto ast = parse(g, "`define ID(x) x\n");
    CHECK(name_of(ast) == "ID");
    auto params = params_of(ast);
    REQUIRE(params);
    REQUIRE(params->data().size() == 1);
    // PARAM_FORMAL now wraps each formal as `{name: "…", [default: …]}`
    // so a `= default_text` clause can hang off the same node.
    auto p0 = std::dynamic_pointer_cast<DictValue>(params->data()[0]);
    REQUIRE(p0); CHECK(str_field(p0, "name") == "x");
    auto body = body_segments_of(g, ast);
    REQUIRE(body);
    REQUIRE(body->data().size() == 1);
    auto seg = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg);
    CHECK(str_field(seg, "type") == "ref");
    CHECK(str_field(seg, "value") == "x");
}

TEST_CASE("sv_pp define: macro with multiple parameters") {
    auto g = load_grammar();
    auto ast = parse(g, "`define ADD(x,y) x + y\n");
    CHECK(name_of(ast) == "ADD");
    auto params = params_of(ast);
    REQUIRE(params);
    REQUIRE(params->data().size() == 2);
    CHECK(str_field(std::dynamic_pointer_cast<DictValue>(params->data()[0]), "name") == "x");
    CHECK(str_field(std::dynamic_pointer_cast<DictValue>(params->data()[1]), "name") == "y");
}

TEST_CASE("sv_pp define: macro params accept canonical SV spacing `(x, y)`") {
    auto g = load_grammar();
    // The wrap-inner PARAMS rule keeps boundary-adjacency strict but
    // makes whitespace around `,` inside the parens transparent.
    auto ast = parse(g, "`define ADD(x, y) x + y\n");
    CHECK(name_of(ast) == "ADD");
    auto params = params_of(ast);
    REQUIRE(params);
    REQUIRE(params->data().size() == 2);
    CHECK(str_field(std::dynamic_pointer_cast<DictValue>(params->data()[0]), "name") == "x");
    CHECK(str_field(std::dynamic_pointer_cast<DictValue>(params->data()[1]), "name") == "y");
}

TEST_CASE("sv_pp define: macro params accept spacing around `,`") {
    auto g = load_grammar();
    // Tab + multi-space around the comma — PARAM_LIST's rule-local
    // `ignore linespace` makes both transparent.
    //
    // Note: whitespace immediately after `(` or before `)` is NOT
    // currently absorbed. The fix needs a save-side change to
    // `?linespace` (track "matched-empty vs skipped" so save can
    // emit nothing when the parse skipped); inlining `?linespace`
    // round-trips as `(x )` instead of `(x)`.
    auto ast = parse(g, "`define F(a ,\tb,c) body\n");
    auto params = params_of(ast);
    REQUIRE(params);
    REQUIRE(params->data().size() == 3);
    CHECK(str_field(std::dynamic_pointer_cast<DictValue>(params->data()[0]), "name") == "a");
    CHECK(str_field(std::dynamic_pointer_cast<DictValue>(params->data()[1]), "name") == "b");
    CHECK(str_field(std::dynamic_pointer_cast<DictValue>(params->data()[2]), "name") == "c");
}

TEST_CASE("sv_pp define: macro params accept arbitrary internal spacing `( a , b , c )`") {
    auto g = load_grammar();
    auto ast = parse(g, "`define F( a , b , c ) body\n");
    CHECK(name_of(ast) == "F");
    auto params = params_of(ast);
    REQUIRE(params);
    REQUIRE(params->data().size() == 3);
    CHECK(str_field(std::dynamic_pointer_cast<DictValue>(params->data()[0]), "name") == "a");
    CHECK(str_field(std::dynamic_pointer_cast<DictValue>(params->data()[1]), "name") == "b");
    CHECK(str_field(std::dynamic_pointer_cast<DictValue>(params->data()[2]), "name") == "c");
}

TEST_CASE("sv_pp define: macro with no parameter list when `(` not adjacent") {
    auto g = load_grammar();
    // Per SV LRM: a space between FOO and `(` means no params — the
    // `(...)` becomes part of the body.
    auto ast = parse(g, "`define FOO (x) y\n");
    CHECK(name_of(ast) == "FOO");
    // No `params` field expected (PARAMS optional was skipped).
    CHECK_FALSE(has_params(ast));
    auto body = body_segments_of(g, ast);
    REQUIRE(body);
    // Body has the `(x) y` content.
    bool found_paren = false;
    for (auto& seg : body->data()) {
        if (auto sv = as_string(seg)) {
            if (sv->data().find("(") != std::string::npos) {
                found_paren = true; break;
            }
        }
    }
    CHECK(found_paren);
}

TEST_CASE("sv_pp define: macro with one parameter round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define ID(x) x\n");
    CHECK(save(g, ast) == "`define ID(x) x\n");
}

TEST_CASE("sv_pp define: macro with multiple parameters round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define ADD(x,y) x + y\n");
    CHECK(save(g, ast) == "`define ADD(x,y) x + y\n");
}

// ─── MACRO_USE arguments inside body ──────────────────────────────

TEST_CASE("sv_pp define: body MACRO_USE with one argument") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `OTHER(x)\n");
    auto body = body_segments_of(g, ast);
    REQUIRE(body);
    REQUIRE(body->data().size() == 1);
    auto seg = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg);
    CHECK(str_field(seg, "type") == "macro_use");
    CHECK(str_field(seg, "name") == "OTHER");
    auto args = std::dynamic_pointer_cast<ArrayValue>(seg->data()["args"]);
    REQUIRE(args);
    REQUIRE(args->data().size() == 1);
    CHECK(arg_text(args->data()[0]) == "x");
}

TEST_CASE("sv_pp define: body MACRO_USE with multiple arguments") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `OTHER(x,y,z)\n");
    auto body = body_segments_of(g, ast);
    REQUIRE(body);
    REQUIRE(body->data().size() == 1);
    auto seg = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg);
    auto args = std::dynamic_pointer_cast<ArrayValue>(seg->data()["args"]);
    REQUIRE(args);
    REQUIRE(args->data().size() == 3);
    CHECK(arg_text(args->data()[0]) == "x");
    CHECK(arg_text(args->data()[1]) == "y");
    CHECK(arg_text(args->data()[2]) == "z");
}

TEST_CASE("sv_pp define: body MACRO_USE binds space-separated args at parse time") {
    auto g = load_grammar();
    // The grammar CAPTURES `\`NAME (args)` — MACRO_ARGS consumes an optional
    // leading `?linespace` before `(`, so a space before the paren still
    // binds the args. Whether those args are USED (function-like) or the
    // parens re-emitted as text (object-like) is the table-dependent
    // DECISION, made at expansion (see the process() tests). The capture is
    // the grammar's job; a `\`A `B` (no paren) still keeps its space because
    // the optional linespace backtracks when no `(` follows.
    auto ast = parse(g, "`define A `OTHER (x)\n");
    auto body = body_segments_of(g, ast);
    REQUIRE(body);
    REQUIRE(body->data().size() >= 1);
    auto seg0 = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg0);
    CHECK(str_field(seg0, "type") == "macro_use");
    CHECK(str_field(seg0, "name") == "OTHER");
    auto args = std::dynamic_pointer_cast<ArrayValue>(seg0->data()["args"]);
    REQUIRE(args);
    REQUIRE(args->data().size() == 1);
    CHECK(arg_text(args->data()[0]) == "x");
}

TEST_CASE("sv_pp define: expansion binds space-separated args for a function-like macro") {
    auto g = load_grammar();
    Preprocessor pp(g);
    // OTHER is function-like, so at expansion the ` (x)` after `\`OTHER`
    // is bound as its argument — the space is permitted (LRM §22.5.1).
    auto out = pp.process(
        "`define OTHER(p) got_p\n"
        "`define A `OTHER (x)\n"
        "`A\n");
    CHECK(out.find("got_p") != std::string::npos);
}

TEST_CASE("sv_pp define: body MACRO_USE single arg round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `OTHER(x)\n");
    CHECK(save(g, ast) == "`define A `OTHER(x)\n");
}

TEST_CASE("sv_pp define: body MACRO_USE multi arg round-trips") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `OTHER(x,y,z)\n");
    CHECK(save(g, ast) == "`define A `OTHER(x,y,z)\n");
}

// TEXT_LINE's leading `!"`endif" !"`else" !"`elsif"` negative-lookahead
// guards are zero-width parse-time assertions — they must emit NOTHING
// on save. Regression guard: a top-level macro-use line previously
// round-tripped with a spurious "`endif`else`elsif" prefix because the
// save engine emitted negative-lookahead Key literals verbatim.
TEST_CASE("scan driver: undefined macro-use line passes through (Leave)") {
    auto g = load_grammar();
    Preprocessor pp(g);  // default on_undefined = Leave
    CHECK(pp.process("`OTHER(x,y,z)\n") == "`OTHER(x,y,z)\n");
}

TEST_CASE("scan driver: plain text passes through verbatim") {
    auto g = load_grammar();
    Preprocessor pp(g);
    CHECK(pp.process("some plain text\n") == "some plain text\n");
}

// ─── Richer MACRO_ARGS — LRM §22.5.1 balanced-token args ───────────────
//
// MACRO_ARGS captures each arg as a balanced-paren token run terminating
// at the top-level `,` or `)`. It uses the `sv_balanced_arg` scanner
// (shared with PARAM_FORMAL_DEFAULT) which follows `()`/`{}`/`[]` nesting
// and skips string literals, so embedded commas and closing parens inside
// nested structures or strings don't end the arg. AST shape is preserved
// (ArrayValue<StringValue>) — each StringValue holds the arg's raw bytes.

// Helper: pull the args array out of a body whose first segment is a
// macro_use dict.
namespace {
std::shared_ptr<ArrayValue> macro_use_args(Grammar& g, const ValuePtr& ast) {
    auto body = body_segments_of(g, ast);
    REQUIRE(body);
    REQUIRE(!body->data().empty());
    auto seg = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg);
    REQUIRE(str_field(seg, "type") == "macro_use");
    auto it = seg->data().find("args");
    REQUIRE(it != seg->data().end());
    auto arr = std::dynamic_pointer_cast<ArrayValue>(it->second);
    REQUIRE(arr);
    return arr;
}
} // namespace

TEST_CASE("sv_pp MACRO_ARGS: numeric literal args") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `OTHER(5, 10)\n");
    auto args = macro_use_args(g, ast);
    REQUIRE(args->data().size() == 2);
    CHECK(arg_text(args->data()[0]) == "5");
    CHECK(arg_text(args->data()[1]) == "10");
}

TEST_CASE("sv_pp MACRO_ARGS: string literal arg") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `OTHER(\"hello, world\")\n");
    auto args = macro_use_args(g, ast);
    REQUIRE(args->data().size() == 1);
    // The arg span captures the string literal verbatim; the embedded
    // `,` inside the quotes does NOT split the args list (sv_balanced_arg
    // skips the whole `"…"` so the inner `,` isn't a top-level separator).
    CHECK(arg_text(args->data()[0]) == "\"hello, world\"");
}

TEST_CASE("sv_pp MACRO_ARGS: expression arg with operators") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `OTHER(a + 1, b * 2)\n");
    auto args = macro_use_args(g, ast);
    REQUIRE(args->data().size() == 2);
    CHECK(arg_text(args->data()[0]) == "a + 1");
    CHECK(arg_text(args->data()[1]) == "b * 2");
}

TEST_CASE("sv_pp MACRO_ARGS: nested parens — `,` inside `()` doesn't split") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `OTHER((a, b), c)\n");
    auto args = macro_use_args(g, ast);
    REQUIRE(args->data().size() == 2);
    CHECK(arg_text(args->data()[0]) == "(a, b)");
    CHECK(arg_text(args->data()[1]) == "c");
}

TEST_CASE("sv_pp MACRO_ARGS: nested function call arg") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `OTHER(foo(1, 2), 3)\n");
    auto args = macro_use_args(g, ast);
    REQUIRE(args->data().size() == 2);
    CHECK(arg_text(args->data()[0]) == "foo(1, 2)");
    CHECK(arg_text(args->data()[1]) == "3");
}

// PARAM_FORMAL_DEFAULT shares the `sv_balanced_arg` scanner with
// MACRO_ARGS, so a formal whose default value is a string literal
// containing a top-level `,` captures the whole string rather than
// truncating at the inner comma. Regression guard for the scanner's
// string-literal skipping.
TEST_CASE("sv_pp DECL: formal default is a string with a comma") {
    auto g = load_grammar();
    auto ast = parse(g, "`define FOO(x = \"a, b\") body\n");
    auto def = std::dynamic_pointer_cast<DictValue>(ast);
    REQUIRE(def);
    auto decl = std::dynamic_pointer_cast<DictValue>(def->data()["decl"]);
    REQUIRE(decl);
    auto params = std::dynamic_pointer_cast<ArrayValue>(decl->data()["params"]);
    REQUIRE(params);
    REQUIRE(params->data().size() == 1);
    auto p0 = std::dynamic_pointer_cast<DictValue>(params->data()[0]);
    REQUIRE(p0);
    CHECK(str_field(p0, "name") == "x");
    auto dflt = std::dynamic_pointer_cast<DictValue>(p0->data()["default"]);
    REQUIRE(dflt);
    CHECK(as_string(dflt->data()["value"])->data() == "\"a, b\"");
}

// End-to-end: the canonical case from earlier debugging — `\`DOUBLE(5)`
// with a numeric literal arg that the previous identifier-only grammar
// dropped to text. Combines: rich MACRO_ARGS (numeric arg captured),
// nested macro_use arg substitution (LRM §22.5.1 — Y substituted in
// `\`INC(Y)`), and use-site whitespace (no longer required to be
// adjacency-tight).
TEST_CASE("Preprocessor::process: nested macro with numeric outer arg") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process(
        "`define INC(X) X + 1\n"
        "`define DOUBLE(Y) `INC(Y) + `INC(Y)\n"
        "`DOUBLE(5)\n"
    );
    CHECK(out.find("5 + 1") != std::string::npos);
    CHECK(out.find("Y + 1") == std::string::npos);
}

// ─── Preprocessor::process() integration ───────────────────────────────
// Wire sv_preprocessor.rawast into the Preprocessor and verify the
// AST shape (segmented body, params field) flows through handle_define
// correctly: the macro lands in the table, and process() emits empty
// output for a pure-define input.

#include <rawast/preprocessor.hpp>

TEST_CASE("Preprocessor::process: `\\`define FOO bar` registers macro") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process("`define FOO bar\n");
    // define emits nothing; the directive is consumed.
    CHECK(out == "");
    REQUIRE(pp.is_defined("FOO"));
    auto m = pp.get_macro("FOO");
    REQUIRE(m);
    CHECK(m->name == "FOO");
    CHECK(m->params.empty());
    CHECK_FALSE(m->is_function_like);
    // Body rendered back to text.
    CHECK(m->body_text() == "bar");
}

TEST_CASE("Preprocessor::process: spaced-param macro expansion end-to-end") {
    auto g = load_grammar();
    Preprocessor pp(g);
    // The whole chain: spaced PARAMS, spaced MACRO_ARGS, expansion.
    auto out = pp.process(
        "`define ADD(x, y) (x + y)\n"
        "`ADD(p, q)\n"
    );
    CHECK(out.find("(p + q)") != std::string::npos);
}

// LRM §22.5.1: at the use site, whitespace between macro name and
// the opening `(` of args is allowed for function-like macros (the
// asymmetric counterpart to the `\`define` adjacency rule). Inside
// a macro body MACRO_USE inherits PP_FILE's `ignore linespace` from
// the surrounding scope dispatch — so `\`INC (Y)` captures (Y) as
// args rather than dropping `(Y)` to text. Relies on the engine
// fix: scope/Raw INNER subparse inherits caller's ignore at
// optional boundaries (`should_skip_optional` predictive seed).
TEST_CASE("Preprocessor::process: use-site whitespace before macro args") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process(
        "`define INC(X) X + 1\n"
        "`define USE(Y) `INC (Y)\n"
        "`USE(foo)\n"
    );
    CHECK(out.find("foo + 1") != std::string::npos);
}

// LRM §22.5.1 / C99 §6.10.3.1: textual arg substitution reaches into
// the args of nested macro calls before they expand. DOUBLE(5)'s body
// `\`INC(Y)` must become `\`INC(5)` before INC fires.
TEST_CASE("Preprocessor::process: outer param substitutes into nested macro_use args") {
    auto g = load_grammar();
    Preprocessor pp(g);
    // Identifier arg, since MACRO_ARGS currently captures identifiers only.
    auto out = pp.process(
        "`define INC(X) X + 1\n"
        "`define DOUBLE(Y) `INC(Y) + `INC(Y)\n"
        "`DOUBLE(foo)\n"
    );
    CHECK(out.find("foo + 1") != std::string::npos);
    CHECK(out.find("Y + 1") == std::string::npos);
}

TEST_CASE("Preprocessor::process: parameterised define registers params") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process("`define ADD(x,y) x + y\n");
    CHECK(out == "");
    REQUIRE(pp.is_defined("ADD"));
    auto m = pp.get_macro("ADD");
    REQUIRE(m);
    REQUIRE(m->params.size() == 2);
    CHECK(m->params[0].name == "x");
    CHECK(m->params[1].name == "y");
    CHECK(m->is_function_like);
    CHECK(m->body_text() == "x + y");
}

// ─── Multi-directive input ─────────────────────────────────────────

TEST_CASE("Preprocessor::process: two defines in sequence both register") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process("`define FOO 1\n`define BAR 2\n");
    REQUIRE(pp.is_defined("FOO"));
    REQUIRE(pp.is_defined("BAR"));
    CHECK(pp.get_macro("FOO")->body_text() == "1");
    CHECK(pp.get_macro("BAR")->body_text() == "2");
}

TEST_CASE("Preprocessor::process: define + text + define interleaved") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process("`define FOO 1\nhello world\n`define BAR 2\n");
    REQUIRE(pp.is_defined("FOO"));
    REQUIRE(pp.is_defined("BAR"));
    // Both macros register regardless of intervening text.
    CHECK(pp.get_macro("FOO")->body_text() == "1");
    CHECK(pp.get_macro("BAR")->body_text() == "2");
}

TEST_CASE("Preprocessor::process: pure text input passes through") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process("just some text\n");
    // No macros registered; the text segment lands as PP_ITEM[0].
    CHECK_FALSE(pp.is_defined("just"));
    // Walker emits the text from the source (newline lost — see grammar
    // comment; multi-line text round-trip is a follow-up).
    CHECK(out.find("just some text") != std::string::npos);
}

TEST_CASE("scan driver: directives register, plain text passes through") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process("`define FOO 1\nhello\n`define BAR 2\n");
    CHECK(pp.is_defined("FOO"));
    CHECK(pp.is_defined("BAR"));
    CHECK(out.find("hello") != std::string::npos);   // pass-through
    CHECK(out.find("`define") == std::string::npos);  // directives consumed
}

// ─── `\`include` end-to-end ──────────────────────────────────────────

TEST_CASE("sv_pp include: grammar parses `\\`include \"path\"`") {
    auto g = load_grammar();
    auto ast = parse(g, "`include \"foo.svh\"\n");
    CHECK(str_field(ast, "type") == "include");
    CHECK(str_field(ast, "path") == "foo.svh");
}

TEST_CASE("Preprocessor::process: include_source callback supplies content") {
    auto g = load_grammar();
    PpOptions opts;
    opts.include_source = [](const std::string& requested,
                              const std::string& /*including*/) -> std::optional<PpIncludeSource> {
        if (requested == "macros.svh") {
            return PpIncludeSource{
                "mem:macros.svh",
                "`define WIDTH 32\n"
            };
        }
        return std::nullopt;
    };
    Preprocessor pp(g, opts);
    pp.process("`include \"macros.svh\"\n");
    REQUIRE(pp.is_defined("WIDTH"));
    CHECK(pp.get_macro("WIDTH")->body_text() == "32");
    // included_files records the canonical id (host-supplied).
    auto& files = pp.included_files();
    REQUIRE(files.size() == 1);
    CHECK(files[0] == "mem:macros.svh");
}

TEST_CASE("Preprocessor::process: multi-include with redefine reprocesses each time") {
    // The motivating use case: same logical header included N times
    // with different macro state before each. The header is a
    // template that emits something depending on the current
    // WIDTH macro. We verify the macro table reflects each include
    // pass independently.
    auto g = load_grammar();
    PpOptions opts;
    int call_count = 0;
    opts.include_source = [&](const std::string& requested,
                               const std::string& /*including*/) -> std::optional<PpIncludeSource> {
        if (requested == "template.svh") {
            ++call_count;
            // Each include of template.svh defines RESULT with the
            // current WIDTH; subsequent includes overwrite RESULT.
            return PpIncludeSource{
                "mem:template.svh",
                "`define RESULT `WIDTH\n"
            };
        }
        return std::nullopt;
    };
    Preprocessor pp(g, opts);
    pp.process(
        "`define WIDTH 8\n"
        "`include \"template.svh\"\n"
        "`define WIDTH 16\n"
        "`include \"template.svh\"\n"
        "`define WIDTH 32\n"
        "`include \"template.svh\"\n"
    );
    // Callback fired three times — once per include directive.
    CHECK(call_count == 3);
    // included_files lists template.svh once (deduped) — the
    // multi-include is for processing, not for the manifest.
    auto& files = pp.included_files();
    REQUIRE(files.size() == 1);
    CHECK(files[0] == "mem:template.svh");
    // WIDTH ended as 32 (last define on the outer stream); RESULT
    // got redefined three times and its current body reflects the
    // last include pass (`WIDTH at body parse time).
    REQUIRE(pp.is_defined("WIDTH"));
    CHECK(pp.get_macro("WIDTH")->body_text() == "32");
    REQUIRE(pp.is_defined("RESULT"));
}

TEST_CASE("Preprocessor::process: include callback nullopt → built-in fallback warning") {
    auto g = load_grammar();
    PpOptions opts;
    opts.include_source = [](const std::string&, const std::string&)
        -> std::optional<PpIncludeSource> { return std::nullopt; };
    // This case tests the WARN path specifically; the default is now Error.
    opts.on_missing_include = PpOnMissingInclude::Warn;
    Preprocessor pp(g, opts);
    pp.process("`include \"does_not_exist.svh\"\n");
    // Built-in fallback runs, finds nothing, warns. The macro
    // table is unchanged.
    CHECK_FALSE(pp.is_defined("WIDTH"));
    auto& warnings = pp.warnings();
    REQUIRE_FALSE(warnings.empty());
    CHECK(warnings.back().message.find("file not found") != std::string::npos);
}

// ─── `\`undef` ────────────────────────────────────────────────────────

TEST_CASE("sv_pp undef: grammar parses `\\`undef NAME`") {
    auto g = load_grammar();
    auto ast = parse(g, "`undef FOO\n");
    CHECK(str_field(ast, "type") == "undef");
    CHECK(name_of(ast) == "FOO");
}

TEST_CASE("Preprocessor::process: define then undef leaves macro undefined") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process("`define FOO 1\n");
    REQUIRE(pp.is_defined("FOO"));
    pp.process("`undef FOO\n");
    CHECK_FALSE(pp.is_defined("FOO"));
}

TEST_CASE("Preprocessor::process: undef of non-existent macro is silent") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process("`undef NEVER_DEFINED\n");
    CHECK_FALSE(pp.is_defined("NEVER_DEFINED"));
    // No warning — undef of an undefined macro is a no-op per LRM.
    auto& warnings = pp.warnings();
    CHECK(warnings.empty());
}

TEST_CASE("Preprocessor::process: define/undef/define cycle ends defined") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process(
        "`define X 1\n"
        "`undef X\n"
        "`define X 2\n"
    );
    REQUIRE(pp.is_defined("X"));
    CHECK(pp.get_macro("X")->body_text() == "2");
}

// ─── `\`ifdef` / `\`ifndef` / `\`endif` ──────────────────────────────

TEST_CASE("sv_pp ifdef: grammar parses `\\`ifdef NAME ... \\`endif`") {
    auto g = load_grammar();
    auto ast = parse(g, "`ifdef FOO\n`endif\n");
    CHECK(str_field(ast, "type") == "ifdef");
    CHECK(str_field(ast, "cond") == "FOO");
}

TEST_CASE("Preprocessor::process: ifdef taken when macro defined") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process(
        "`define FLAG 1\n"
        "`ifdef FLAG\n"
        "`define INSIDE 1\n"
        "`endif\n"
    );
    REQUIRE(pp.is_defined("FLAG"));
    CHECK(pp.is_defined("INSIDE"));
}

TEST_CASE("sv_pp ifdef: `ifdef/`elsif chain parses as ifdef, not `if") {
    // Regression: `ifdef ... `elsif ... `endif was mis-parsed as an `if
    // (cond "def A") because IFDEF lacked elsif support and PEG backtracked
    // into IF, which matches the `if prefix of `ifdef.
    auto g = load_grammar();
    auto ast = parse(g, "`ifdef A\n`define X 1\n`elsif B\n`define Y 1\n`endif\n");
    CHECK(str_field(ast, "type") == "ifdef");
    CHECK(str_field(ast, "cond") == "A");
}

TEST_CASE("Preprocessor::process: `ifdef/`elsif selects by definedness") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process(
        "`define B 1\n"
        "`ifdef A\n"
        "`define X 1\n"
        "`elsif B\n"
        "`define Y 1\n"
        "`else\n"
        "`define Z 1\n"
        "`endif\n"
    );
    CHECK_FALSE(pp.is_defined("X"));  // A undefined -> ifdef body skipped
    CHECK(pp.is_defined("Y"));        // B defined   -> elsif taken
    CHECK_FALSE(pp.is_defined("Z"));  // else not taken
}

TEST_CASE("Preprocessor::process: `ifdef/`elsif falls to else when none defined") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process(
        "`ifdef A\n`define X 1\n`elsif B\n`define Y 1\n`else\n`define Z 1\n`endif\n"
    );
    CHECK_FALSE(pp.is_defined("X"));
    CHECK_FALSE(pp.is_defined("Y"));
    CHECK(pp.is_defined("Z"));
}

TEST_CASE("Preprocessor::process: ifdef skipped when macro undefined") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process(
        "`ifdef NEVER\n"
        "`define INSIDE 1\n"
        "`endif\n"
    );
    CHECK_FALSE(pp.is_defined("NEVER"));
    CHECK_FALSE(pp.is_defined("INSIDE"));
}

TEST_CASE("Preprocessor::process: ifndef inverts ifdef") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process(
        "`ifndef NEVER\n"
        "`define DEFAULT 1\n"
        "`endif\n"
    );
    // ifndef takes its body when the macro is NOT defined.
    CHECK(pp.is_defined("DEFAULT"));
}

TEST_CASE("Preprocessor::process: ifdef else-branch taken when condition false") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process(
        "`ifdef NEVER\n"
        "`define TAKEN 1\n"
        "`else\n"
        "`define ALT 1\n"
        "`endif\n"
    );
    CHECK_FALSE(pp.is_defined("TAKEN"));
    CHECK(pp.is_defined("ALT"));
}

TEST_CASE("Preprocessor::process: nested ifdef inside taken body") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process(
        "`define OUTER 1\n"
        "`define INNER 1\n"
        "`ifdef OUTER\n"
        "`ifdef INNER\n"
        "`define BOTH 1\n"
        "`endif\n"
        "`endif\n"
    );
    CHECK(pp.is_defined("BOTH"));
}

TEST_CASE("Preprocessor::process: nested ifdef inside skipped body is skipped") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process(
        "`define INNER 1\n"
        "`ifdef NEVER_OUTER\n"
        "`ifdef INNER\n"
        "`define BOTH 1\n"
        "`endif\n"
        "`endif\n"
    );
    // Outer skipped means inner's body doesn't execute even though
    // INNER is defined.
    CHECK_FALSE(pp.is_defined("BOTH"));
}

// ─── `\`if` / `\`elsif` / `\`else` / `\`endif` ───────────────────────
//
// IF_HEAD/ELSIF capture cond as RAW TEXT (robust — any condition
// preprocesses). The engine evaluates it on demand with the BUILT-IN
// default (C `#if` semantics, parsing the cond text through PP_EXPR and
// resolving macros against its own table) — on by default, no host
// wiring. A cond outside the PP_EXPR subset is undecidable, not a parse
// failure. The tests below drive that built-in path with real conditions.

// Raw `*` cond capture keeps the leading space after the directive
// keyword (the driver / eval_cond_default trim it). Compare trimmed.
static std::string trimmed_cond(const ValuePtr& ast) {
    std::string s = str_field(ast, "cond");
    std::size_t b = s.find_first_not_of(" \t");
    std::size_t e = s.find_last_not_of(" \t");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

TEST_CASE("sv_pp if: `if parses as a flat construct with raw cond text") {
    auto g = load_grammar();
    auto ast = parse(g, "`if FOO\n");
    CHECK(str_field(ast, "type") == "pp_if");
    // Flat scan model: cond is RAW TEXT; the built-in evaluator subparses
    // it through PP_COND on demand (no #subparse at grammar time).
    CHECK(trimmed_cond(ast) == "FOO");
}

TEST_CASE("sv_pp: `if / `elsif / `endif each parse as flat constructs") {
    auto g = load_grammar();
    // Each conditional directive is its own PP_CONSTRUCT; the scan driver
    // pairs them via its emit/skip stack (no BODY nesting in the grammar).
    CHECK(str_field(parse(g, "`if FOO\n"), "type") == "pp_if");
    CHECK(str_field(parse(g, "`elsif defined(BAR)\n"), "type") == "elsif");
    CHECK(str_field(parse(g, "`endif\n"), "type") == "endif");
    CHECK(trimmed_cond(parse(g, "`if FOO\n")) == "FOO");
    CHECK(trimmed_cond(parse(g, "`elsif defined(BAR)\n")) == "defined(BAR)");
}

TEST_CASE("Preprocessor::process: `if takes first branch when cond true") {
    auto g = load_grammar();
    Preprocessor pp(g);   // built-in default evaluator
    pp.process(
        "`define COND 1\n"
        "`if COND\n"
        "`define A 1\n"
        "`endif\n"
    );
    CHECK(pp.is_defined("A"));
}

TEST_CASE("Preprocessor::process: `if skips body when cond false") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process(
        "`if defined(MISSING)\n"
        "`define A 1\n"
        "`endif\n"
    );
    CHECK_FALSE(pp.is_defined("A"));
}

TEST_CASE("Preprocessor::process: `elsif taken when `if false") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process(
        "`define B 1\n"
        "`if defined(A)\n"
        "`define X 1\n"
        "`elsif defined(B)\n"
        "`define Y 1\n"
        "`endif\n"
    );
    CHECK_FALSE(pp.is_defined("X"));
    CHECK(pp.is_defined("Y"));
}

TEST_CASE("Preprocessor::process: first matching branch wins, rest skipped") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process(
        "`define A 1\n"
        "`define B 1\n"
        "`if defined(A)\n"
        "`define X 1\n"
        "`elsif defined(B)\n"
        "`define Y 1\n"
        "`endif\n"
    );
    CHECK(pp.is_defined("X"));
    CHECK_FALSE(pp.is_defined("Y"));
}

TEST_CASE("Preprocessor::process: `else taken when all branches false") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process(
        "`if defined(A)\n"
        "`define X 1\n"
        "`elsif defined(B)\n"
        "`define Y 1\n"
        "`else\n"
        "`define Z 1\n"
        "`endif\n"
    );
    CHECK_FALSE(pp.is_defined("X"));
    CHECK_FALSE(pp.is_defined("Y"));
    CHECK(pp.is_defined("Z"));
}

// ─── Built-in default evaluator (C `#if` semantics) ──────────────────
//
// No use_default_expr_eval, no separate grammar: the engine evaluates
// the structured cond against its own macro table whenever expr_eval is
// unset.

TEST_CASE("built-in eval: defined(FOO) — true when defined") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process("`define FOO 1\n`if defined(FOO)\n`define TAKEN 1\n`endif\n");
    CHECK(pp.is_defined("TAKEN"));
}

TEST_CASE("built-in eval: defined(MISSING) — false when undefined") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process("`if defined(MISSING)\n`define TAKEN 1\n`endif\n");
    CHECK_FALSE(pp.is_defined("TAKEN"));
}

TEST_CASE("built-in eval: !defined(LEGACY) gates a branch") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process("`if !defined(LEGACY)\n`define MODERN 1\n`endif\n");
    CHECK(pp.is_defined("MODERN"));
}

TEST_CASE("built-in eval: defined(A) && defined(B) — both required") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process("`define A 1\n`define B 1\n"
               "`if defined(A) && defined(B)\n`define BOTH 1\n`endif\n");
    CHECK(pp.is_defined("BOTH"));
}

TEST_CASE("built-in eval: defined(A) && defined(B) — false when B missing") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process("`define A 1\n"
               "`if defined(A) && defined(B)\n`define BOTH 1\n`endif\n");
    CHECK_FALSE(pp.is_defined("BOTH"));
}

TEST_CASE("built-in eval: bare ref FOO — true when defined, false when not") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process("`define FOO 1\n`if FOO\n`define TAKEN 1\n`endif\n");
    CHECK(pp.is_defined("TAKEN"));

    Preprocessor pp2(g);
    pp2.process("`if BAR\n`define TAKEN 1\n`endif\n");   // BAR undefined → 0
    CHECK_FALSE(pp2.is_defined("TAKEN"));
}

TEST_CASE("built-in eval: integer comparison `\\`if W > 16`") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process("`define W 32\n`if W > 16\n`define BIG 1\n`endif\n");
    CHECK(pp.is_defined("BIG"));
}

TEST_CASE("built-in eval: a condition folds macros through the ONE expansion") {
    auto g = load_grammar();
    // Nested macro in a bare-ident condition (W -> `\`X -> 16): resolved via
    // the same expansion mechanism as body text, not a shallow body read.
    {
        Preprocessor pp(g);
        pp.process("`define X 16\n`define W `X\n"
                   "`if W > 8\n`define TAKEN 1\n`endif\n");
        CHECK(pp.is_defined("TAKEN"));
    }
    // A backtick macro USE in the condition itself is expanded before eval.
    {
        Preprocessor pp(g);
        pp.process("`define W 16\n`if `W > 8\n`define TAKEN 1\n`endif\n");
        CHECK(pp.is_defined("TAKEN"));
    }
}

// End-to-end: the full SV expression surface folds in `\`if — the same
// COND_EXPR the parser uses, evaluated by default_pp_expr_eval. Each
// takes the first branch iff the constant condition is non-zero.
TEST_CASE("built-in eval: shift / bitwise / ternary / sized literals in `\\`if") {
    auto g = load_grammar();
    auto takes = [&](const std::string& cond) {
        Preprocessor pp(g);
        pp.process("`if " + cond + "\n`define TAKEN 1\n`endif\n");
        return pp.is_defined("TAKEN");
    };
    // Shifts.
    CHECK(takes("1 << 3"));
    CHECK_FALSE(takes("0 << 3"));
    CHECK(takes("16 >> 2"));
    CHECK_FALSE(takes("1 >> 3"));
    // Bitwise.
    CHECK(takes("6 & 2"));
    CHECK_FALSE(takes("6 & 1"));
    CHECK(takes("5 | 0"));
    CHECK_FALSE(takes("6 ^ 6"));
    CHECK(takes("~0"));
    // Ternary selects the branch, then takes its truthiness.
    CHECK_FALSE(takes("1 ? 0 : 1"));
    CHECK(takes("0 ? 0 : 1"));
    CHECK(takes("2 ? 5 : 0"));
    // Sized / based literals, incl. signed sign-extension and truncation.
    CHECK(takes("8'hFF"));
    CHECK_FALSE(takes("8'h00"));
    CHECK(takes("4'sd8 < 0"));       // signed 4-bit 8 == -8
    CHECK_FALSE(takes("4'h10 != 0")); // truncates to 4 bits → 0
    // Composed.
    CHECK(takes("(1 << 4) | 1"));
    CHECK(takes("(2 << 2) == 8"));
}

// ─── Top-level macro expansion ──────────────────────────────────────
//
// TEXT_LINE is now a scope-array of MACRO_USE inside a sequence-dict,
// so `\`FOO` and `\`FOO(a, b)` at top level (or mid-line) surface as
// macro_use dicts that the walker expands through the macro table.

TEST_CASE("Preprocessor::process: whole-line `\\`MACRO expands to body") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process(
        "`define FOO bar\n"
        "`FOO\n"
    );
    CHECK(out.find("bar") != std::string::npos);
    CHECK(out.find("`FOO") == std::string::npos);
}

TEST_CASE("Preprocessor::process: mid-line `\\`MACRO embedded in surrounding text") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process(
        "`define WIDTH 32\n"
        "wire [`WIDTH-1:0] x;\n"
    );
    // Macro substituted in place; surrounding text preserved.
    CHECK(out.find("wire [32-1:0] x;") != std::string::npos);
}

TEST_CASE("Preprocessor::process: two macros on one line both expand") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process(
        "`define A alpha\n"
        "`define B beta\n"
        "x = `A + `B;\n"
    );
    CHECK(out.find("x = alpha + beta;") != std::string::npos);
}

TEST_CASE("Preprocessor::process: text-only line passes through unchanged") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process("module top; endmodule\n");
    CHECK(out.find("module top; endmodule") != std::string::npos);
}

TEST_CASE("Preprocessor::process: parameterised macro `\\`FOO(a,b) expands with substitution") {
    auto g = load_grammar();
    Preprocessor pp(g);
    // PARAMS / MACRO_ARGS currently require no whitespace around the
    // `,` separator — a known v1 limitation. Test uses the canonical
    // tight form.
    auto out = pp.process(
        "`define ADD(x,y) (x + y)\n"
        "`ADD(p,q)\n"
    );
    CHECK(out.find("(p + q)") != std::string::npos);
}

// ─── Pass-through for unknown directives ─────────────────────────────
//
// SV files are full of directives we don't (and may never) interpret:
// `\`timescale`, `\`celldefine`, `\`line`, `\`begin_keywords`, etc.
// They surface as undefined MACRO_USE nodes with their trailing args
// captured as text segments; on_undefined::Leave (the default) emits
// the `\`NAME` verbatim and the text segments follow as-is, so the
// whole directive line round-trips. No warning is emitted in Leave
// mode, so real corpus files don't drown the warnings buffer.

TEST_CASE("Preprocessor::process: `\\`timescale passes through verbatim") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process(
        "`timescale 1ns/1ps\n"
        "module top; endmodule\n"
    );
    CHECK(out.find("`timescale") != std::string::npos);
    CHECK(out.find("1ns/1ps") != std::string::npos);
    CHECK(out.find("module top") != std::string::npos);
    CHECK(pp.warnings().empty());
}

TEST_CASE("Preprocessor::process: `\\`celldefine / `\\`endcelldefine (no args) pass through") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process(
        "`celldefine\n"
        "module cell; endmodule\n"
        "`endcelldefine\n"
    );
    CHECK(out.find("`celldefine") != std::string::npos);
    CHECK(out.find("`endcelldefine") != std::string::npos);
    CHECK(out.find("module cell") != std::string::npos);
}

TEST_CASE("Preprocessor::process: `\\`default_nettype none passes through") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process("`default_nettype none\n");
    CHECK(out.find("`default_nettype") != std::string::npos);
    CHECK(out.find("none") != std::string::npos);
}

TEST_CASE("Preprocessor::process: `\\`begin_keywords \"1800-2017\" passes through") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process(
        "`begin_keywords \"1800-2017\"\n"
        "module top; endmodule\n"
        "`end_keywords\n"
    );
    CHECK(out.find("`begin_keywords") != std::string::npos);
    CHECK(out.find("\"1800-2017\"") != std::string::npos);
    CHECK(out.find("`end_keywords") != std::string::npos);
}

TEST_CASE("Preprocessor::process: `\\`include with angle-bracket form falls through to pass-through") {
    auto g = load_grammar();
    Preprocessor pp(g);
    // INCLUDE rule only matches `"..."` form; `<...>` is a v1
    // limitation. PEG ordered choice backtracks the failed INCLUDE
    // attempt and TEXT_LINE picks up `\`include` as undefined
    // MACRO_USE, so the line still round-trips verbatim.
    auto out = pp.process("`include <foo.svh>\n");
    CHECK(out.find("`include") != std::string::npos);
    CHECK(out.find("<foo.svh>") != std::string::npos);
}

TEST_CASE("Preprocessor::process: unknown directive followed by real define still expands the macro") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process(
        "`timescale 1ns/1ps\n"
        "`define WIDTH 32\n"
        "wire [`WIDTH-1:0] x;\n"
    );
    // Pass-through directive preserved AND real macro expansion still
    // works after it — proves the unknown directive doesn't poison
    // following state.
    CHECK(out.find("`timescale") != std::string::npos);
    CHECK(out.find("wire [32-1:0] x;") != std::string::npos);
}

TEST_CASE("Preprocessor::process: macro use after `\\`ifdef-taken branch") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process(
        "`define FLAG 1\n"
        "`define VAL 42\n"
        "`ifdef FLAG\n"
        "x = `VAL;\n"
        "`endif\n"
    );
    CHECK(out.find("x = 42;") != std::string::npos);
}

// ─── process() → Stream handoff ──────────────────────────────────────
// (The old Mode-1 parse()->AST / Mode-2 preprocess(ast)->Stream API is
// gone with the whole-file-AST walker; the preprocessor is a scan pass.)

TEST_CASE("Preprocessor::process expands macros in a declaration") {
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process(
        "`define WIDTH 32\n"
        "wire [`WIDTH-1:0] x;\n"
    );
    // Macro registered; `define line consumed; WIDTH replaced by 32.
    CHECK(pp.macros().count("WIDTH") == 1);
    CHECK(out.find("`define") == std::string::npos);
    CHECK(out.find("wire [32-1:0] x;") != std::string::npos);
}

TEST_CASE("Preprocessor::process output feeds a Stream that survives a move") {
    // The expanded text can be handed to a host grammar via a Stream;
    // moving the Stream keeps its source bytes alive.
    auto g = load_grammar();
    Preprocessor pp(g);
    auto out = pp.process("`define X 7\n`X\n");
    CHECK(out.find("7") != std::string::npos);

    Stream s1 = Stream::from_string(out);
    Stream s2 = std::move(s1);
    std::string drained;
    while (auto c = s2.reader().get()) drained.push_back(*c);
    CHECK(drained.find("7") != std::string::npos);
}

// ─── Provenance / source maps ─────────────────────────────────────────
// The scan driver records emit-time spans so stack_at can map an output
// offset back to its source origin — through includes. Regression guard:
// the scan-driven rewrite once silently dropped every record_span call.

TEST_CASE("Preprocessor: stack_at maps output offsets back to source") {
    auto g = load_grammar();
    Preprocessor pp(g);
    std::string src = "`define W 8\nwire [`W-1:0] x = zork;\n";
    auto out = pp.process(src);
    auto z = out.find("zork");
    REQUIRE(z != std::string::npos);
    auto frames = pp.stack_at(z);
    REQUIRE(!frames.empty());
    // Some frame must land exactly on `zork` in the ORIGINAL source — the
    // `\`W` expansion shifts offsets, so a raw output offset wouldn't.
    bool mapped = false;
    for (const auto& f : frames)
        if (f.offset + 4 <= src.size() && src.compare(f.offset, 4, "zork") == 0)
            mapped = true;
    CHECK(mapped);
}

TEST_CASE("Preprocessor: stack_at backtraces through an `\\`include`") {
    auto g = load_grammar();
    PpOptions opts;
    opts.splice = true;
    opts.include_source = [](const std::string& req, const std::string&)
        -> std::optional<PpIncludeSource> {
        if (req == "hdr.svh")
            return PpIncludeSource{"hdr.svh", "wire hdr_sig;\nBADTOK\n"};
        return std::nullopt;
    };
    Preprocessor pp(g, std::move(opts));
    auto out = pp.process("`include \"hdr.svh\"\nassign top = q;\n");
    auto b = out.find("BADTOK");
    REQUIRE(b != std::string::npos);
    auto frames = pp.stack_at(b);
    // The chain names the included file AND the including root — a backtrace.
    bool in_include = false, in_root = false;
    for (const auto& f : frames) {
        if (f.where == "hdr.svh")  in_include = true;
        if (f.where == "<input>")  in_root = true;
    }
    CHECK(in_include);
    CHECK(in_root);
}

// A macro expansion's bytes have no per-byte source; an error anywhere
// inside the expanded text should point at the `\`NAME` use site, not
// drift linearly past it into following source. The expansion span is
// `collapse`, so stack_at maps every byte in it to the single use-site
// offset.
TEST_CASE("Preprocessor: stack_at collapses a macro expansion to its use site") {
    auto g = load_grammar();
    Preprocessor pp(g);
    // Expansion (20 bytes) is far longer than the `\`LONG` token, so a
    // linear map would overshoot the use site by up to 20 bytes.
    std::string src = "`define LONG averylongreplacement\n`LONG x;\n";
    auto out = pp.process(src);
    auto e = out.find("averylongreplacement");
    REQUIRE(e != std::string::npos);
    auto deep = e + 15;                         // 15 bytes into the expansion
    auto use = src.find("`LONG");               // the use site in source
    REQUIRE(use != std::string::npos);
    auto frames = pp.stack_at(deep);
    REQUIRE(!frames.empty());
    // The source-file frame must land EXACTLY on the use site — not
    // `use + 15` (the pre-collapse linear drift).
    bool checked = false;
    for (const auto& f : frames) {
        if (f.where == "<input>") {
            CHECK(f.offset == use);
            checked = true;
        }
    }
    CHECK(checked);
}
