#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_sv.hpp>
#include <rawast/preprocessor.hpp>

#include <fstream>

using namespace rawast;

namespace {

// Minimal preprocessor-style grammar built directly in code — a
// pass-through that just consumes any input as one chunk. Lets the
// Preprocessor skeleton be exercised without a full systemverilog
// grammar in place (that lands in a later commit).
//
// The walker isn't wired yet, so process() returns input verbatim;
// the only behavior under test here is the class lifecycle and
// inspection accessors.
Grammar make_passthrough_grammar() {
    register_std_parser_group();
    Grammar g;
    // No rules needed — process() doesn't reach the parse path yet.
    return g;
}

// The mini-preprocessor tests exercise the real scan-driven driver
// (src/preprocessor.cpp) against the systemverilog grammar — the driver
// is grammar-agnostic; PP_CONSTRUCT is all it needs. They use only
// standard SV preprocessor syntax (object-like `define NAME BODY,
// conditionals, include, macro use).
Grammar make_mini_preprocessor() {
    register_std_parser_group();
    register_sv_parser_group();
    Grammar g;
    auto r = load_rawast_grammar_from_file(g, "grammars/systemverilog.rawast");
    REQUIRE_MESSAGE(r, "loading systemverilog.rawast failed: "
                       << (r ? "" : r.error()));
    return g;
}

// A SECOND backtick preprocessor grammar (tests/backtick_pp.rawast),
// distinct from systemverilog.rawast. Proves the scan driver is coupled
// to the grammar CONTRACT (PP_CONSTRUCT + MACRO_BODY rule names, the
// `type=` role vocabulary, the value shapes) — not to the SV grammar.
Grammar make_backtick_pp() {
    register_std_parser_group();
    register_sv_parser_group();  // reuse the shared sv_* terminals
    Grammar g;
    auto r = load_rawast_grammar_from_file(g, "tests/backtick_pp.rawast");
    REQUIRE_MESSAGE(r, "loading tests/backtick_pp.rawast failed: "
                       << (r ? "" : r.error()));
    return g;
}

} // namespace

TEST_CASE("Preprocessor: default-constructed options carry the documented defaults") {
    PpOptions opts;
    CHECK(opts.predefined.empty());
    CHECK(opts.include_paths.empty());
    CHECK(opts.splice == false);
    CHECK(opts.on_undefined == PpOnUndefined::Leave);
    CHECK(opts.max_expansion_depth == 200);
    CHECK(opts.trace == false);
}

TEST_CASE("Preprocessor: skeleton process() returns input verbatim (walker not yet wired)") {
    auto g = make_passthrough_grammar();
    Preprocessor pp(g);
    CHECK(pp.process("hello world") == "hello world");
    CHECK(pp.macros().empty());
    CHECK(pp.included_files().empty());
    CHECK(pp.warnings().empty());
}

TEST_CASE("Preprocessor: is_defined / get_macro on empty state") {
    auto g = make_passthrough_grammar();
    Preprocessor pp(g);
    CHECK_FALSE(pp.is_defined("FOO"));
    CHECK(pp.get_macro("FOO") == nullptr);
}

TEST_CASE("Preprocessor: reset clears accumulated state") {
    auto g = make_passthrough_grammar();
    Preprocessor pp(g);
    // No way to populate state yet (walker not wired); just verify
    // the lifecycle method runs without crashing on empty state.
    pp.reset();
    CHECK(pp.macros().empty());
    CHECK(pp.included_files().empty());
}

TEST_CASE("Preprocessor: snapshot/restore round-trips state") {
    auto g = make_passthrough_grammar();
    Preprocessor pp(g);
    auto snap = pp.snapshot();
    CHECK(snap.macros.empty());
    CHECK(snap.included_files.empty());
    pp.restore(std::move(snap));
    CHECK(pp.macros().empty());
}

TEST_CASE("Preprocessor: PpOptions can be passed through the constructor") {
    auto g = make_passthrough_grammar();
    PpOptions opts;
    opts.splice = true;
    opts.on_undefined = PpOnUndefined::Warn;
    opts.max_expansion_depth = 50;
    opts.include_paths = {"./rtl", "./uvm"};
    Preprocessor pp(g, std::move(opts));
    // No accessor for options yet (they're internal); just verify
    // construction succeeded and inspection accessors work.
    CHECK(pp.macros().empty());
}

TEST_CASE("Preprocessor: process_file returns empty + records warning when file missing") {
    auto g = make_passthrough_grammar();
    Preprocessor pp(g);
    auto out = pp.process_file("/nonexistent/path/should-not-exist.sv");
    CHECK(out.empty());
    REQUIRE(pp.warnings().size() == 1);
    CHECK(pp.warnings()[0].message.find("failed to open") != std::string::npos);
}

// ─── Walker integration tests via a minimal in-test grammar ─────────────

TEST_CASE("mini_preprocessor: `define registers an object-like macro") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);
    pp.process("`define WIDTH BAR\n");
    REQUIRE(pp.is_defined("WIDTH"));
    auto m = pp.get_macro("WIDTH");
    REQUIRE(m != nullptr);
    CHECK(m->name == "WIDTH");
    CHECK(m->body_text() == "BAR");
    CHECK_FALSE(m->is_function_like);
}

TEST_CASE("mini_preprocessor: `undef removes a previously defined macro") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);
    pp.process("`define FOO BAR\n`undef FOO\n");
    CHECK_FALSE(pp.is_defined("FOO"));
}

TEST_CASE("mini_preprocessor: `ifdef takes the body when macro is defined") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "`define DBG ON\n"
        "`ifdef DBG\n"
        "active\n"
        "`endif\n");
    CHECK(out.find("active") != std::string::npos);
}

TEST_CASE("mini_preprocessor: `ifdef drops the body when macro is undefined") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "`ifdef MISSING\n"
        "should_not_appear\n"
        "`endif\n");
    CHECK(out.find("should_not_appear") == std::string::npos);
}

TEST_CASE("mini_preprocessor: `ifndef inverts the condition") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "`ifndef MISSING\n"
        "fallback\n"
        "`endif\n");
    CHECK(out.find("fallback") != std::string::npos);
}

TEST_CASE("mini_preprocessor: `else branch taken when condition is false") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "`ifdef MISSING\n"
        "yes_branch\n"
        "`else\n"
        "no_branch\n"
        "`endif\n");
    CHECK(out.find("no_branch") != std::string::npos);
    CHECK(out.find("yes_branch") == std::string::npos);
}

TEST_CASE("mini_preprocessor: multiple `define + `undef round-trip macro state") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);
    pp.process("`define A ONE\n");
    pp.process("`define B TWO\n");
    CHECK(pp.macros().size() == 2);
    CHECK(pp.is_defined("A"));
    CHECK(pp.is_defined("B"));
    pp.process("`undef A\n");
    CHECK_FALSE(pp.is_defined("A"));
    CHECK(pp.is_defined("B"));
}

TEST_CASE("mini_preprocessor: reset clears macro state") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);
    pp.process("`define X Y\n");
    REQUIRE(pp.is_defined("X"));
    pp.reset();
    CHECK_FALSE(pp.is_defined("X"));
    CHECK(pp.macros().empty());
}

TEST_CASE("mini_preprocessor: `MACRO expands to its body") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "`define WIDTH OUTW\n"
        "`WIDTH\n");
    // `WIDTH should be replaced by its body OUTW.
    CHECK(out.find("OUTW") != std::string::npos);
}

TEST_CASE("mini_preprocessor: undefined `MACRO falls through PpOnUndefined::Leave") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);  // default: PpOnUndefined::Leave
    auto out = pp.process("`MISSING\n");
    // Leave policy: undefined ref emits the source verbatim.
    CHECK(out.find("`MISSING") != std::string::npos);
}

TEST_CASE("mini_preprocessor: undefined `MACRO under PpOnUndefined::Empty drops it") {
    auto g = make_mini_preprocessor();
    PpOptions opts;
    opts.on_undefined = PpOnUndefined::Empty;
    Preprocessor pp(g, std::move(opts));
    auto out = pp.process("`MISSING\n");
    // Empty policy: the macro reference expands to empty.
    CHECK(out.find("`MISSING") == std::string::npos);
    CHECK(out.find("MISSING") == std::string::npos);
}

TEST_CASE("mini_preprocessor: undefined `MACRO under PpOnUndefined::Warn records a warning") {
    auto g = make_mini_preprocessor();
    PpOptions opts;
    opts.on_undefined = PpOnUndefined::Warn;
    Preprocessor pp(g, std::move(opts));
    pp.process("`MISSING\n");
    REQUIRE(pp.warnings().size() >= 1);
    bool found = false;
    for (const auto& w : pp.warnings()) {
        if (w.message.find("MISSING") != std::string::npos) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("mini_preprocessor: nested `ifdef inside another `ifdef") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "`define OUTER YES\n"
        "`define INNER YES\n"
        "`ifdef OUTER\n"
        "`ifdef INNER\n"
        "deepest\n"
        "`endif\n"
        "`endif\n");
    CHECK(out.find("deepest") != std::string::npos);
}

TEST_CASE("mini_preprocessor: nested `ifdef — inner suppressed by outer") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "`ifdef MISSING\n"
        "`ifdef ANYTHING\n"
        "hidden\n"
        "`endif\n"
        "`endif\n");
    CHECK(out.find("hidden") == std::string::npos);
}

TEST_CASE("mini_preprocessor: PpOptions.predefined seeds the macro table") {
    auto g = make_mini_preprocessor();
    PpOptions opts;
    opts.predefined = "`define PRESET ON\n";
    Preprocessor pp(g, std::move(opts));
    // After construction, predefined is processed — PRESET should be
    // visible to subsequent process() calls.
    CHECK(pp.is_defined("PRESET"));
    auto m = pp.get_macro("PRESET");
    REQUIRE(m != nullptr);
    CHECK(m->body_text() == "ON");
}

TEST_CASE("mini_preprocessor: snapshot/restore round-trips active macro state") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);
    pp.process("`define A FIRST\n`define B SECOND\n");
    auto snap = pp.snapshot();
    pp.process("`undef A\n`define C THIRD\n");
    CHECK_FALSE(pp.is_defined("A"));
    CHECK(pp.is_defined("C"));
    pp.restore(std::move(snap));
    CHECK(pp.is_defined("A"));
    CHECK(pp.is_defined("B"));
    CHECK_FALSE(pp.is_defined("C"));   // C was added after snapshot
}

// ─── Driver tests: undefined_handler + `if / expr_eval ───────────────
// These drive the real scan-driven preprocessor (via process() on
// directive text) to pin the callback + conditional-evaluation
// behaviour. (The former synthesized-AST / process_ast tests are gone
// with the whole-file-AST walker.)

TEST_CASE("pp: undefined_handler fires and supplies expansion") {
    auto g = make_mini_preprocessor();
    PpOptions opts;
    bool fired = false;
    opts.undefined_handler = [&](const std::string& name,
                                 const std::vector<std::string>&)
        -> std::optional<std::string> {
        fired = true;
        CHECK(name == "GIT_HASH");
        return std::string("abc123");
    };
    Preprocessor pp(g, std::move(opts));
    auto out = pp.process("`GIT_HASH\n");
    CHECK(fired);
    CHECK(out.find("abc123") != std::string::npos);
}

TEST_CASE("pp: undefined_handler returning nullopt falls through to policy") {
    auto g = make_mini_preprocessor();
    PpOptions opts;
    opts.on_undefined = PpOnUndefined::Empty;
    int call_count = 0;
    opts.undefined_handler = [&](const std::string&,
                                 const std::vector<std::string>&)
        -> std::optional<std::string> {
        ++call_count;
        return std::nullopt;
    };
    Preprocessor pp(g, std::move(opts));
    auto out = pp.process("`UNKNOWN\n");
    CHECK(call_count == 1);
    CHECK(out.find("UNKNOWN") == std::string::npos);  // Empty ate the site
}

TEST_CASE("pp: undefined_handler skipped when macro is actually defined") {
    auto g = make_mini_preprocessor();
    PpOptions opts;
    bool fired = false;
    opts.undefined_handler = [&](const std::string&,
                                 const std::vector<std::string>&)
        -> std::optional<std::string> {
        fired = true;
        return std::string("FROM_CALLBACK");
    };
    Preprocessor pp(g, std::move(opts));
    auto out = pp.process("`define X FROM_DEFINE\n`X\n");
    CHECK_FALSE(fired);
    CHECK(out.find("FROM_DEFINE") != std::string::npos);
    CHECK(out.find("FROM_CALLBACK") == std::string::npos);
}

TEST_CASE("pp: undefined_handler receives raw args") {
    auto g = make_mini_preprocessor();
    PpOptions opts;
    std::vector<std::string> captured;
    opts.undefined_handler = [&](const std::string& name,
                                 const std::vector<std::string>& args)
        -> std::optional<std::string> {
        CHECK(name == "JOIN");
        captured = args;
        return std::string("joined");
    };
    Preprocessor pp(g, std::move(opts));
    auto out = pp.process("`JOIN(a,b,c)\n");
    REQUIRE(captured.size() == 3);
    CHECK(captured[0] == "a");
    CHECK(captured[1] == "b");
    CHECK(captured[2] == "c");
    CHECK(out.find("joined") != std::string::npos);
}

TEST_CASE("pp: `if takes first branch when expr_eval returns true") {
    auto g = make_mini_preprocessor();
    PpOptions opts;
    opts.expr_eval = [](const ValuePtr& cond) -> ValuePtr {
        auto s = as_string(cond);
        return (s && s->data() == "ALPHA") ? true_value() : false_value();
    };
    Preprocessor pp(g, std::move(opts));
    auto out = pp.process(
        "`if ALPHA\nA_TAKEN\n`else\nELSE_TAKEN\n`endif\n");
    CHECK(out.find("A_TAKEN") != std::string::npos);
    CHECK(out.find("ELSE_TAKEN") == std::string::npos);
}

TEST_CASE("pp: `if falls to else_branch when no branch true") {
    auto g = make_mini_preprocessor();
    PpOptions opts;
    opts.expr_eval = [](const ValuePtr&) -> ValuePtr { return false_value(); };
    Preprocessor pp(g, std::move(opts));
    auto out = pp.process(
        "`if X\nX_BODY\n`else\nFALLBACK\n`endif\n");
    CHECK(out.find("FALLBACK") != std::string::npos);
    CHECK(out.find("X_BODY") == std::string::npos);
}

TEST_CASE("pp: `if/`elsif chain picks the first true branch") {
    auto g = make_mini_preprocessor();
    PpOptions opts;
    opts.expr_eval = [](const ValuePtr& cond) -> ValuePtr {
        auto s = as_string(cond);
        return (s && s->data() == "B") ? true_value() : false_value();
    };
    Preprocessor pp(g, std::move(opts));
    auto out = pp.process(
        "`if A\nA_BODY\n`elsif B\nB_BODY\n`elsif C\nC_BODY\n`endif\n");
    CHECK(out.find("B_BODY") != std::string::npos);
    CHECK(out.find("A_BODY") == std::string::npos);
    CHECK(out.find("C_BODY") == std::string::npos);
}

TEST_CASE("pp: `if with an undefined ref is false (built-in eval)") {
    auto g = make_mini_preprocessor();
    Preprocessor pp(g);   // built-in eval on by default
    // X undefined -> 0 -> false, branch skipped. Decidable -> no warning.
    auto out = pp.process("`if X\nSHOULD_NOT_APPEAR\n`endif\n");
    CHECK(out.find("SHOULD_NOT_APPEAR") == std::string::npos);
}

TEST_CASE("pp: expr_eval returning Undefined warns and skips (default policy)") {
    auto g = make_mini_preprocessor();
    PpOptions opts;
    opts.expr_eval = [](const ValuePtr&) -> ValuePtr {
        return undefined_value();
    };
    Preprocessor pp(g, std::move(opts));
    auto out = pp.process(
        "`if UNKNOWN\nBODY\n`else\nFALLBACK\n`endif\n");
    CHECK(out.find("BODY") == std::string::npos);
    CHECK(out.find("FALLBACK") != std::string::npos);
    REQUIRE_FALSE(pp.warnings().empty());
}

TEST_CASE("pp: expr_eval receives the raw condition text") {
    // In the scan-driven model a custom expr_eval is handed the raw
    // condition text (the built-in eval_cond_default subparses it
    // through COND_EXPR itself). Host evaluators that want structure
    // re-parse via the grammar.
    auto g = make_mini_preprocessor();
    PpOptions opts;
    std::string seen;
    opts.expr_eval = [&](const ValuePtr& cond) -> ValuePtr {
        if (auto s = as_string(cond)) seen = s->data();
        return true_value();
    };
    Preprocessor pp(g, std::move(opts));
    auto out = pp.process("`if FOO && BAR\nBODY\n`endif\n");
    CHECK(seen == "FOO && BAR");
    CHECK(out.find("BODY") != std::string::npos);
}


// ─── Grammar-agnostic driver: a non-SV backtick grammar drives it ───
// If the scan driver ever re-hardcodes an SV assumption (a rule name,
// a keyword, a value shape beyond the documented contract), these break.

TEST_CASE("generic driver: a second backtick grammar drives the same Preprocessor") {
    auto g = make_backtick_pp();

    SUBCASE("object-like define + macro use (keyword is `def, not `define)") {
        Preprocessor pp(g);
        // Dispatch keys on type="define" (the ROLE), not the spelling.
        CHECK(pp.process("`def FOO 1\nint x = `FOO;\n") == "int x = 1;\n");
    }
    SUBCASE("function-like define with arguments") {
        Preprocessor pp(g);
        CHECK(pp.process("`def ADD(a,b) (a+b)\ny = `ADD(2,3);\n") == "y = (2+3);\n");
    }
    SUBCASE("nested / recursive expansion") {
        Preprocessor pp(g);
        CHECK(pp.process("`def W hello\n`def MSG `W world\nm = `MSG;\n")
              == "m = hello world;\n");
    }
    SUBCASE("undefined macro passes through (Leave)") {
        Preprocessor pp(g);  // default on_undefined = Leave
        CHECK(pp.process("y = `NOPE;\n") == "y = `NOPE;\n");
    }
}

TEST_CASE("Preprocessor: enum round-trips") {
    CHECK(parse_pp_on_undefined("leave").value() == PpOnUndefined::Leave);
    CHECK(parse_pp_on_undefined("error").value() == PpOnUndefined::Error);
    CHECK(parse_pp_on_undefined("warn").value() ==  PpOnUndefined::Warn);
    CHECK(parse_pp_on_undefined("empty").value() == PpOnUndefined::Empty);
    CHECK_FALSE(parse_pp_on_undefined("bogus").has_value());
    CHECK(to_string(PpOnUndefined::Leave) == "leave");
    CHECK(to_string(PpOnUndefined::Error) == "error");
    CHECK(to_string(PpOnUndefined::Warn)  == "warn");
    CHECK(to_string(PpOnUndefined::Empty) == "empty");
}
