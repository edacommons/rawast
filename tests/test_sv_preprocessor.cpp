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
#include <rawast/value.hpp>

#include <sstream>

using namespace rawast;

namespace {

Grammar load_grammar() {
    register_std_parser_group();
    Grammar g;
    auto r = load_rawast_grammar_from_file(g, "grammars/sv_preprocessor.rawast");
    REQUIRE_MESSAGE(r, "loading sv_preprocessor.rawast failed: "
                       << (r ? "" : r.error()));
    return g;
}

// Full PP_FILE parse result (an ArrayValue of PP_ITEMs).
ValuePtr parse_file(Grammar& g, const std::string& src) {
    std::istringstream is{src};
    StreamReader sr{is};
    auto r = g.parse(sr);
    REQUIRE_MESSAGE(r, "parse failed for '" << src << "': "
                       << (r ? "" : r.error().message));
    return *r;
}

// Convenience: for tests that expect a single PP_ITEM, extract it
// from the PP_FILE wrapper.
ValuePtr parse(Grammar& g, const std::string& src) {
    auto pp_file = parse_file(g, src);
    auto arr = std::dynamic_pointer_cast<ArrayValue>(pp_file);
    REQUIRE(arr);
    REQUIRE(arr->data().size() == 1);
    return arr->data()[0];
}

std::string save(Grammar& g, ValuePtr v) {
    // The grammar's top is PP_FILE (an ArrayValue). When tests pass
    // a single PP_ITEM dict via the `parse()` shortcut, wrap it back
    // up so save sees the PP_FILE shape it expects.
    if (std::dynamic_pointer_cast<DictValue>(v)) {
        auto wrapped = std::make_shared<ArrayValue>();
        wrapped->data().push_back(std::move(v));
        v = std::move(wrapped);
    }
    std::ostringstream out;
    auto r = g.save(out, std::move(v));
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

std::shared_ptr<ArrayValue> body_of(const ValuePtr& ast) {
    auto d = std::dynamic_pointer_cast<DictValue>(ast);
    REQUIRE(d);
    auto it = d->data().find("body");
    REQUIRE(it != d->data().end());
    return std::dynamic_pointer_cast<ArrayValue>(it->second);
}

} // namespace

// ─── Top-level shape ──────────────────────────────────────────────

TEST_CASE("sv_pp define: empty body → {type:'define', name, body:[]}") {
    auto g = load_grammar();
    auto ast = parse(g, "`define FOO\n");
    CHECK(str_field(ast, "type") == "define");
    CHECK(str_field(ast, "name") == "FOO");
    auto body = body_of(ast);
    REQUIRE(body);
    CHECK(body->data().empty());
}

TEST_CASE("sv_pp define: simple text body → one StringValue segment") {
    auto g = load_grammar();
    auto ast = parse(g, "`define FOO bar\n");
    CHECK(str_field(ast, "name") == "FOO");
    auto body = body_of(ast);
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
    auto body = body_of(ast);
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
    auto body = body_of(ast);
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
    auto body = body_of(ast);
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
    auto body = body_of(ast);
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
    // Canonical save emits the identifier's trailing `space` even on
    // empty bodies — semantically equivalent to the parse input.
    CHECK(save(g, ast) == "`define FOO \n");
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
    CHECK(str_field(ast, "name") == "ID");
    auto params = std::dynamic_pointer_cast<ArrayValue>(
        std::dynamic_pointer_cast<DictValue>(ast)->data()["params"]);
    REQUIRE(params);
    REQUIRE(params->data().size() == 1);
    auto p0 = as_string(params->data()[0]);
    REQUIRE(p0); CHECK(p0->data() == "x");
    auto body = body_of(ast);
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
    CHECK(str_field(ast, "name") == "ADD");
    auto params = std::dynamic_pointer_cast<ArrayValue>(
        std::dynamic_pointer_cast<DictValue>(ast)->data()["params"]);
    REQUIRE(params);
    REQUIRE(params->data().size() == 2);
    CHECK(as_string(params->data()[0])->data() == "x");
    CHECK(as_string(params->data()[1])->data() == "y");
}

TEST_CASE("sv_pp define: macro with no parameter list when `(` not adjacent") {
    auto g = load_grammar();
    // Per SV LRM: a space between FOO and `(` means no params — the
    // `(...)` becomes part of the body.
    auto ast = parse(g, "`define FOO (x) y\n");
    CHECK(str_field(ast, "name") == "FOO");
    // No `params` field expected (PARAMS optional was skipped).
    auto d = std::dynamic_pointer_cast<DictValue>(ast);
    CHECK(d->data().find("params") == d->data().end());
    auto body = body_of(ast);
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
    auto body = body_of(ast);
    REQUIRE(body);
    REQUIRE(body->data().size() == 1);
    auto seg = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg);
    CHECK(str_field(seg, "type") == "macro_use");
    CHECK(str_field(seg, "name") == "OTHER");
    auto args = std::dynamic_pointer_cast<ArrayValue>(seg->data()["args"]);
    REQUIRE(args);
    REQUIRE(args->data().size() == 1);
    CHECK(as_string(args->data()[0])->data() == "x");
}

TEST_CASE("sv_pp define: body MACRO_USE with multiple arguments") {
    auto g = load_grammar();
    auto ast = parse(g, "`define A `OTHER(x,y,z)\n");
    auto body = body_of(ast);
    REQUIRE(body);
    REQUIRE(body->data().size() == 1);
    auto seg = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg);
    auto args = std::dynamic_pointer_cast<ArrayValue>(seg->data()["args"]);
    REQUIRE(args);
    REQUIRE(args->data().size() == 3);
    CHECK(as_string(args->data()[0])->data() == "x");
    CHECK(as_string(args->data()[1])->data() == "y");
    CHECK(as_string(args->data()[2])->data() == "z");
}

TEST_CASE("sv_pp define: body MACRO_USE with no args when `(` not adjacent") {
    auto g = load_grammar();
    // Space between backtick-ident and `(` means no args — the
    // `(...)` is text in the surrounding body.
    auto ast = parse(g, "`define A `OTHER (x)\n");
    auto body = body_of(ast);
    REQUIRE(body);
    // First segment is macro_use without args
    auto seg0 = std::dynamic_pointer_cast<DictValue>(body->data()[0]);
    REQUIRE(seg0);
    CHECK(str_field(seg0, "type") == "macro_use");
    CHECK(str_field(seg0, "name") == "OTHER");
    CHECK(seg0->data().find("args") == seg0->data().end());
    // Subsequent segments include the literal ` (x)` text + refs.
    REQUIRE(body->data().size() >= 2);
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
    CHECK(m->body == "bar");
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
    CHECK(m->params[0] == "x");
    CHECK(m->params[1] == "y");
    CHECK(m->is_function_like);
    CHECK(m->body == "x + y");
}

// ─── Multi-directive input ─────────────────────────────────────────

TEST_CASE("Preprocessor::process: two defines in sequence both register") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process("`define FOO 1\n`define BAR 2\n");
    REQUIRE(pp.is_defined("FOO"));
    REQUIRE(pp.is_defined("BAR"));
    CHECK(pp.get_macro("FOO")->body == "1");
    CHECK(pp.get_macro("BAR")->body == "2");
}

TEST_CASE("Preprocessor::process: define + text + define interleaved") {
    auto g = load_grammar();
    Preprocessor pp(g);
    pp.process("`define FOO 1\nhello world\n`define BAR 2\n");
    REQUIRE(pp.is_defined("FOO"));
    REQUIRE(pp.is_defined("BAR"));
    // Both macros register regardless of intervening text.
    CHECK(pp.get_macro("FOO")->body == "1");
    CHECK(pp.get_macro("BAR")->body == "2");
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

TEST_CASE("PP_FILE: parse_file returns array of PP_ITEMs") {
    auto g = load_grammar();
    auto pp_file = parse_file(g, "`define FOO 1\nhello\n`define BAR 2\n");
    auto arr = std::dynamic_pointer_cast<ArrayValue>(pp_file);
    REQUIRE(arr);
    CHECK(arr->data().size() == 3);
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
    CHECK(pp.get_macro("WIDTH")->body == "32");
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
    CHECK(pp.get_macro("WIDTH")->body == "32");
    REQUIRE(pp.is_defined("RESULT"));
}

TEST_CASE("Preprocessor::process: include callback nullopt → built-in fallback warning") {
    auto g = load_grammar();
    PpOptions opts;
    opts.include_source = [](const std::string&, const std::string&)
        -> std::optional<PpIncludeSource> { return std::nullopt; };
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
    CHECK(str_field(ast, "name") == "FOO");
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
    CHECK(pp.get_macro("X")->body == "2");
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
// Cond is captured as raw text (StringValue). The host's
// PpOptions::expr_eval callback is responsible for parsing the cond
// text into an AST and evaluating it — typically by composing
// sv_pp_expr.rawast with default_pp_expr_eval. The tests below stub
// expr_eval with a trivial truthiness check ("TRUE"/"FALSE") so
// they exercise the walker's branch-selection logic in isolation.

TEST_CASE("sv_pp if: grammar parses `\\`if EXPR ... \\`endif`") {
    auto g = load_grammar();
    auto ast = parse(g, "`if FOO\n`endif\n");
    CHECK(str_field(ast, "type") == "if");
    auto d = std::dynamic_pointer_cast<DictValue>(ast);
    REQUIRE(d);
    auto br_it = d->data().find("branches");
    REQUIRE(br_it != d->data().end());
    auto branches = std::dynamic_pointer_cast<ArrayValue>(br_it->second);
    REQUIRE(branches);
    REQUIRE(branches->data().size() == 1);
    CHECK(str_field(branches->data()[0], "cond") == "FOO");
}

TEST_CASE("sv_pp if: grammar parses `\\`if EXPR ... \\`elsif EXPR ... \\`endif`") {
    auto g = load_grammar();
    auto ast = parse(g,
        "`if FOO\n"
        "`elsif BAR\n"
        "`elsif BAZ\n"
        "`endif\n");
    auto d = std::dynamic_pointer_cast<DictValue>(ast);
    REQUIRE(d);
    auto branches = std::dynamic_pointer_cast<ArrayValue>(
        d->data().find("branches")->second);
    REQUIRE(branches);
    REQUIRE(branches->data().size() == 3);
    CHECK(str_field(branches->data()[0], "cond") == "FOO");
    CHECK(str_field(branches->data()[1], "cond") == "BAR");
    CHECK(str_field(branches->data()[2], "cond") == "BAZ");
}

namespace {

// Stub expr_eval: cond text equals "TRUE" → true, else false.
// Real callers will compose sv_pp_expr parsing + default_pp_expr_eval.
auto stub_truthy_eval() {
    return [](const ValuePtr& cond) -> std::optional<bool> {
        auto s = as_string(cond);
        if (!s) return std::nullopt;
        return s->data() == "TRUE";
    };
}

} // namespace

TEST_CASE("Preprocessor::process: `if takes first branch when expr is true") {
    auto g = load_grammar();
    PpOptions opts;
    opts.expr_eval = stub_truthy_eval();
    Preprocessor pp(g, opts);
    pp.process(
        "`if TRUE\n"
        "`define A 1\n"
        "`endif\n"
    );
    CHECK(pp.is_defined("A"));
}

TEST_CASE("Preprocessor::process: `if skips body when expr is false") {
    auto g = load_grammar();
    PpOptions opts;
    opts.expr_eval = stub_truthy_eval();
    Preprocessor pp(g, opts);
    pp.process(
        "`if FALSE\n"
        "`define A 1\n"
        "`endif\n"
    );
    CHECK_FALSE(pp.is_defined("A"));
}

TEST_CASE("Preprocessor::process: `elsif taken when `if false") {
    auto g = load_grammar();
    PpOptions opts;
    opts.expr_eval = stub_truthy_eval();
    Preprocessor pp(g, opts);
    pp.process(
        "`if FALSE\n"
        "`define A 1\n"
        "`elsif TRUE\n"
        "`define B 1\n"
        "`endif\n"
    );
    CHECK_FALSE(pp.is_defined("A"));
    CHECK(pp.is_defined("B"));
}

TEST_CASE("Preprocessor::process: first matching branch wins, rest skipped") {
    auto g = load_grammar();
    PpOptions opts;
    opts.expr_eval = stub_truthy_eval();
    Preprocessor pp(g, opts);
    pp.process(
        "`if TRUE\n"
        "`define A 1\n"
        "`elsif TRUE\n"
        "`define B 1\n"
        "`endif\n"
    );
    CHECK(pp.is_defined("A"));
    CHECK_FALSE(pp.is_defined("B"));
}

TEST_CASE("Preprocessor::process: `else taken when all branches false") {
    auto g = load_grammar();
    PpOptions opts;
    opts.expr_eval = stub_truthy_eval();
    Preprocessor pp(g, opts);
    pp.process(
        "`if FALSE\n"
        "`define A 1\n"
        "`elsif FALSE\n"
        "`define B 1\n"
        "`else\n"
        "`define C 1\n"
        "`endif\n"
    );
    CHECK_FALSE(pp.is_defined("A"));
    CHECK_FALSE(pp.is_defined("B"));
    CHECK(pp.is_defined("C"));
}

TEST_CASE("Preprocessor::process: `if without expr_eval callback warns + skips") {
    auto g = load_grammar();
    // No expr_eval set; walker emits a warning and treats branch as false.
    Preprocessor pp(g);
    pp.process(
        "`if FOO\n"
        "`define A 1\n"
        "`endif\n"
    );
    CHECK_FALSE(pp.is_defined("A"));
    CHECK_FALSE(pp.warnings().empty());
}
