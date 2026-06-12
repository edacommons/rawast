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
// Preprocessor skeleton be exercised without a full sv_preprocessor
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

// ─── Walker integration tests via sv_preprocessor.rawast ────────────────

namespace {

Grammar load_sv_preprocessor() {
    register_std_parser_group();
    register_sv_parser_group();
    Grammar g;
    auto r = load_rawast_grammar_from_file(g, "grammars/sv_preprocessor.rawast");
    REQUIRE_MESSAGE(r, "loading sv_preprocessor.rawast failed: "
                       << (r ? "" : r.error()));
    return g;
}

} // namespace

TEST_CASE("sv_preprocessor: `define registers an object-like macro") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define WIDTH 32\n");
    REQUIRE(pp.is_defined("WIDTH"));
    auto m = pp.get_macro("WIDTH");
    REQUIRE(m != nullptr);
    CHECK(m->name == "WIDTH");
    CHECK(m->body == "32");
    CHECK_FALSE(m->is_function_like);
}

TEST_CASE("sv_preprocessor: `undef removes a previously defined macro") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define FOO 1\n`undef FOO\n");
    CHECK_FALSE(pp.is_defined("FOO"));
}

TEST_CASE("sv_preprocessor: `ifdef takes the body when macro is defined") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "`define RVFI\n"
        "`ifdef RVFI\n"
        "  output logic rvfi_valid,\n"
        "`endif\n");
    CHECK(out.find("rvfi_valid") != std::string::npos);
}

TEST_CASE("sv_preprocessor: `ifdef drops the body when macro is undefined") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "`ifdef NEVER_DEFINED\n"
        "  output logic should_not_appear,\n"
        "`endif\n");
    CHECK(out.find("should_not_appear") == std::string::npos);
}

TEST_CASE("sv_preprocessor: `ifndef takes the body when macro is undefined") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "`ifndef NEVER_DEFINED\n"
        "  output logic should_appear,\n"
        "`endif\n");
    CHECK(out.find("should_appear") != std::string::npos);
}

TEST_CASE("sv_preprocessor: `ifdef + `else takes the else branch when macro undefined") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "`ifdef NEVER_DEFINED\n"
        "  body_branch\n"
        "`else\n"
        "  else_branch\n"
        "`endif\n");
    CHECK(out.find("else_branch") != std::string::npos);
    CHECK(out.find("body_branch") == std::string::npos);
}

TEST_CASE("sv_preprocessor: nested `ifdef nests correctly") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "`define OUTER\n"
        "`ifdef OUTER\n"
        "  outer_visible\n"
        "  `ifdef INNER\n"
        "    inner_hidden\n"
        "  `endif\n"
        "  outer_after\n"
        "`endif\n");
    CHECK(out.find("outer_visible") != std::string::npos);
    CHECK(out.find("outer_after") != std::string::npos);
    CHECK(out.find("inner_hidden") == std::string::npos);
}

TEST_CASE("sv_preprocessor: predefined option seeds macro state at construction") {
    auto g = load_sv_preprocessor();
    PpOptions opts;
    opts.predefined = "`define DSIM\n";
    Preprocessor pp(g, std::move(opts));
    CHECK(pp.is_defined("DSIM"));
    auto out = pp.process(
        "`ifdef DSIM\n"
        "  sim_only\n"
        "`endif\n");
    CHECK(out.find("sim_only") != std::string::npos);
}

// ─── Function-like macros ───────────────────────────────────────────────

TEST_CASE("function-like: define captures params and body separately") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define MAX(a, b) ((a) > (b) ? (a) : (b))\n");
    auto m = pp.get_macro("MAX");
    REQUIRE(m != nullptr);
    CHECK(m->is_function_like);
    REQUIRE(m->params.size() == 2);
    CHECK(m->params[0] == "a");
    CHECK(m->params[1] == "b");
    CHECK(m->body == "((a) > (b) ? (a) : (b))");
}

TEST_CASE("function-like: object-like define unchanged") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define WIDTH 32\n");
    auto m = pp.get_macro("WIDTH");
    REQUIRE(m != nullptr);
    CHECK_FALSE(m->is_function_like);
    CHECK(m->params.empty());
    CHECK(m->body == "32");
}

TEST_CASE("function-like: macro use with args expands via substitution") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define SQ(x) ((x) * (x))\n");
    auto out = pp.process("`SQ(5)\n");
    // Substitution: x → 5, so body becomes "((5) * (5))".
    CHECK(out.find("((5) * (5))") != std::string::npos);
}

TEST_CASE("function-like: multi-arg substitution") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define ADD(a, b) (a + b)\n");
    auto out = pp.process("`ADD(x, y)\n");
    CHECK(out.find("(x + y)") != std::string::npos);
}

TEST_CASE("function-like: arity mismatch warns and emits body verbatim") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define TWO(a, b) (a + b)\n");
    pp.process("`TWO(only_one)\n");
    REQUIRE(pp.warnings().size() >= 1);
    bool found_arity_warn = false;
    for (const auto& w : pp.warnings()) {
        if (w.message.find("expects 2 args") != std::string::npos) {
            found_arity_warn = true;
            break;
        }
    }
    CHECK(found_arity_warn);
}

TEST_CASE("function-like: object-like macro use without args still works") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define PI 3.14\n");
    auto out = pp.process("`PI\n");
    CHECK(out.find("3.14") != std::string::npos);
}

TEST_CASE("function-like: substitution only replaces whole identifier tokens") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    // Body contains identifier `a` and `aaa`; only `a` substitutes.
    pp.process("`define M(a) a + aaa\n");
    auto out = pp.process("`M(42)\n");
    // After substitution: "42 + aaa" — `aaa` is NOT a match for `a`
    CHECK(out.find("42 + aaa") != std::string::npos);
    CHECK(out.find("42aa") == std::string::npos);  // not greedy
}

TEST_CASE("function-like: PP_MACRO_USE does not eat `endif terminator") {
    auto g = load_sv_preprocessor();
    PpOptions opts;
    opts.predefined = "`define OUTER\n";
    Preprocessor pp(g, std::move(opts));
    auto out = pp.process(
        "`ifdef OUTER\n"
        "  inside\n"
        "`endif\n"
        "after\n");
    // Both `inside` and `after` should appear — if PP_MACRO_USE
    // matched `endif as a macro name, the body would extend
    // through the rest of input.
    CHECK(out.find("inside") != std::string::npos);
    CHECK(out.find("after") != std::string::npos);
}

// ─── Token paste + stringification ──────────────────────────────────────

TEST_CASE("paste: `` joins adjacent tokens after substitution") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define MK(x) prefix_``x\n");
    auto out = pp.process("`MK(value)\n");
    // x → value, then prefix_``value → prefix_value
    CHECK(out.find("prefix_value") != std::string::npos);
    CHECK(out.find("`") == std::string::npos);  // no backticks survive
}

TEST_CASE("paste: `` between two params") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define JOIN(a, b) a``b\n");
    auto out = pp.process("`JOIN(foo, bar)\n");
    CHECK(out.find("foobar") != std::string::npos);
}

TEST_CASE("stringify: `\"...`\" becomes a string literal") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define STR(x) `\"value: x`\"\n");
    auto out = pp.process("`STR(42)\n");
    // x substitutes inside `\"...`\"; outer markers become `"..."`.
    CHECK(out.find("\"value: 42\"") != std::string::npos);
}

TEST_CASE("stringify: works on object-like macros too") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define TAG `\"hello`\"\n");
    auto out = pp.process("`TAG\n");
    CHECK(out.find("\"hello\"") != std::string::npos);
}

TEST_CASE("paste + stringify: combined in UVM-style macro") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    // UVM-style: builds a stringified prefix-suffix combo.
    pp.process("`define INFO(tag, msg) print(`\"tag``: msg`\")\n");
    auto out = pp.process("`INFO(MOD, ready)\n");
    // tag → MOD, msg → ready. Then `\"MOD``: ready`\" → "MOD: ready".
    CHECK(out.find("\"MOD: ready\"") != std::string::npos);
}

// ─── \`include directive ────────────────────────────────────────────────

namespace {

// Write a file to /tmp for the include tests; auto-cleans nothing
// (CI tmpfs lifecycle handles it) — tests are self-contained so
// stale files don't confuse runs.
std::string write_tmp(const std::string& name, const std::string& content) {
    auto path = std::string("/tmp/rawast_pp_test_") + name;
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

} // namespace

TEST_CASE("include: side-effects-only mode accumulates macros, emits nothing") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    auto inc_path = write_tmp("ssfx_inner.svh",
                              "`define FROM_INCLUDE 42\n");
    auto out = pp.process(
        "before\n`include \"" + inc_path + "\"\nafter\n");
    // Side-effects-only: included text NOT spliced into output.
    CHECK(out.find("`define") == std::string::npos);
    CHECK(out == "before\nafter\n");
    // The macro IS now defined.
    CHECK(pp.is_defined("FROM_INCLUDE"));
    CHECK(pp.get_macro("FROM_INCLUDE")->body == "42");
}

TEST_CASE("include: splice mode emits processed text") {
    auto g = load_sv_preprocessor();
    PpOptions opts;
    opts.splice = true;
    Preprocessor pp(g, std::move(opts));
    auto inc_path = write_tmp("splice_inner.svh",
                              "`define X 1\nspliced_content\n");
    auto out = pp.process(
        "before\n`include \"" + inc_path + "\"\nafter\n");
    // Splice: the included file's processed content lands in output.
    CHECK(out.find("spliced_content") != std::string::npos);
    CHECK(out.find("before") != std::string::npos);
    CHECK(out.find("after") != std::string::npos);
    // Macros still defined.
    CHECK(pp.is_defined("X"));
}

TEST_CASE("include: tracks the included file in included_files") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    auto inc_path = write_tmp("tracked_inner.svh", "`define A 1\n");
    pp.process("`include \"" + inc_path + "\"\n");
    bool found = false;
    for (const auto& f : pp.included_files()) {
        if (f.find("tracked_inner.svh") != std::string::npos) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("include: nested include — header's defines flow up") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    auto deep_path = write_tmp("deep_inner.svh", "`define DEEPLY_DEFINED 1\n");
    auto wrap_path = write_tmp("wrap_inner.svh",
                               "`include \"" + deep_path + "\"\n");
    pp.process("`include \"" + wrap_path + "\"\n");
    // The deeply-nested define should be visible in the outer scope.
    CHECK(pp.is_defined("DEEPLY_DEFINED"));
}

TEST_CASE("include: missing file emits warning, parse continues") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "before\n`include \"/nonexistent/path/does_not_exist.svh\"\nafter\n");
    // Continues past the failed include.
    CHECK(out.find("before") != std::string::npos);
    CHECK(out.find("after") != std::string::npos);
    // Warning was recorded.
    bool found_warning = false;
    for (const auto& w : pp.warnings()) {
        if (w.message.find("file not found") != std::string::npos) {
            found_warning = true;
            break;
        }
    }
    CHECK(found_warning);
}

TEST_CASE("include: respects include_paths search list") {
    auto g = load_sv_preprocessor();
    auto inc_path = write_tmp("paths_inner.svh", "`define PATH_SEARCH 1\n");
    // Use a bare filename in `\`include`; the file lives in /tmp.
    PpOptions opts;
    opts.include_paths = {"/tmp"};
    Preprocessor pp(g, std::move(opts));
    pp.process("`include \"rawast_pp_test_paths_inner.svh\"\n");
    CHECK(pp.is_defined("PATH_SEARCH"));
}

// ─── Recursive expansion + blue painting ────────────────────────────────

TEST_CASE("recursive: object-like macro body references another macro") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define INNER 42\n");
    pp.process("`define OUTER `INNER\n");
    auto out = pp.process("`OUTER\n");
    CHECK(out.find("42") != std::string::npos);
    CHECK(out.find("`INNER") == std::string::npos);
}

TEST_CASE("recursive: function-like wraps function-like") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define INNER(x) ((x))\n");
    pp.process("`define OUTER(y) `INNER(y)\n");
    auto out = pp.process("`OUTER(7)\n");
    // OUTER(7) → INNER(7) → ((7))
    CHECK(out.find("((7))") != std::string::npos);
    CHECK(out.find("`INNER") == std::string::npos);
}

TEST_CASE("recursive: deep object-like chain expands all the way") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define A `B\n");
    pp.process("`define B `C\n");
    pp.process("`define C done\n");
    auto out = pp.process("`A\n");
    CHECK(out.find("done") != std::string::npos);
    CHECK(out.find("`B") == std::string::npos);
    CHECK(out.find("`C") == std::string::npos);
}

TEST_CASE("recursive: direct self-reference is broken by blue paint") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define LOOP `LOOP\n");
    // Without blue paint this would infinite-loop. With it, the
    // inner `\`LOOP` falls back to verbatim.
    auto out = pp.process("`LOOP\n");
    CHECK(out.find("`LOOP") != std::string::npos);  // verbatim survival
}

TEST_CASE("recursive: mutual recursion breaks at the second hop") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("`define A `B\n");
    pp.process("`define B `A\n");
    auto out = pp.process("`A\n");
    // A expands to body "`B" → B's body is "`A" → A is in active set → verbatim "`A".
    CHECK(out.find("`A") != std::string::npos);
}

TEST_CASE("recursive: max depth aborts with a warning, leaves verbatim") {
    PpOptions opts;
    opts.max_expansion_depth = 3;
    auto g = load_sv_preprocessor();
    Preprocessor pp(g, std::move(opts));
    // Chain `A → `B → `C → `D — three macro chains, depth budget = 3.
    pp.process("`define A `B\n");
    pp.process("`define B `C\n");
    pp.process("`define C `D\n");
    pp.process("`define D done\n");
    pp.process("`A\n");
    bool found_depth_warn = false;
    for (const auto& w : pp.warnings()) {
        if (w.message.find("max_expansion_depth") != std::string::npos) {
            found_depth_warn = true;
            break;
        }
    }
    CHECK(found_depth_warn);
}

TEST_CASE("recursive: undefined macro inside body passes through verbatim") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    // Note: body must NOT start with `(` or the walker would
    // interpret it as a function-like parameter list (the SV LRM
    // distinction relies on whitespace the grammar's ignore policy
    // has already eaten — function-like vs object-like with a
    // body that begins with `(` is the known Phase 2.1 ambiguity).
    pp.process("`define WRAPPER value_`UNKNOWN_done\n");
    auto out = pp.process("`WRAPPER\n");
    // UNKNOWN_done never defined → kept as `\`UNKNOWN_done` so the
    // host parser can decide what to do.
    CHECK(out.find("`UNKNOWN_done") != std::string::npos);
}

// ─── Source map tests ───────────────────────────────────────────────────

TEST_CASE("source map: plain text passes through with provenance") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process("hello world\n");
    CHECK(out == "hello world\n");
    // At least one output span exists covering the emitted text.
    bool found_output = false;
    for (const auto& s : pp.spans()) {
        if (s.out_offset != Span::NoOutput) {
            found_output = true;
            CHECK(s.length == 12);  // "hello world\n"
            CHECK(s.parent_offset == 0);
        }
    }
    CHECK(found_output);
}

TEST_CASE("source map: stack_at returns the input file as root") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("first line\nsecond line\n");
    auto stack = pp.stack_at(5);   // somewhere in "first line"
    REQUIRE(stack.size() >= 1);
    // Root frame is the input.
    CHECK(stack.back().where == "<input>");
}

TEST_CASE("source map: stack_at maps to the correct source offset") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("abcdef\n");
    auto stack = pp.stack_at(3);
    REQUIRE(!stack.empty());
    // Direct passthrough — output offset 3 corresponds to source offset 3.
    CHECK(stack.back().offset == 3);
}

TEST_CASE("source map: directive lines are consumed without output span") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    // "`define X 1\nhello\n" — directive is consumed (no output), then "hello\n" emitted.
    auto out = pp.process("`define X 1\nhello\n");
    CHECK(out == "hello\n");
    // The "hello\n" emitted at out_offset=0 should map to source offset 12
    // (past "`define X 1\n" which is 12 bytes).
    auto stack = pp.stack_at(0);
    REQUIRE(!stack.empty());
    CHECK(stack.back().offset == 12);
}

TEST_CASE("source map: dropped ifdef body does not emit spans") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    auto out = pp.process(
        "before\n"
        "`ifdef NEVER\n"
        "  dropped_line\n"
        "`endif\n"
        "after\n");
    CHECK(out == "before\nafter\n");
    // "after\n" should map back to its actual source position
    // past the ifdef block. The block consumes:
    //   "before\n"        bytes 0-6
    //   "`ifdef NEVER\n"  bytes 7-19
    //   "  dropped_line\n" bytes 20-34
    //   "`endif\n"        bytes 35-41
    //   "after\n"         bytes 42-47
    auto pos = out.find("after");
    auto stack = pp.stack_at(pos);
    REQUIRE(!stack.empty());
    CHECK(stack.back().offset == 42);
}

TEST_CASE("source map: stack_at returns empty for out-of-range offset") {
    auto g = load_sv_preprocessor();
    Preprocessor pp(g);
    pp.process("text\n");
    auto stack = pp.stack_at(9999);
    CHECK(stack.empty());
}

TEST_CASE("source map: ifdef-taken body preserves provenance per-chunk") {
    PpOptions opts;
    opts.predefined = "`define RVFI\n";
    auto g = load_sv_preprocessor();
    Preprocessor pp(g, std::move(opts));

    auto out = pp.process(
        "before\n"
        "`ifdef RVFI\n"
        "  inside\n"
        "`endif\n");
    auto pos = out.find("inside");
    auto stack = pp.stack_at(pos);
    REQUIRE(!stack.empty());
    // Source byte offset of "inside" in this input:
    //   "before\n"      bytes 0-6
    //   "`ifdef RVFI\n" bytes 7-18 (12 bytes)
    //   "  inside\n"    bytes 19-27 — "inside" starts at byte 21.
    CHECK(stack.back().offset == 21);
}

TEST_CASE("PpRole: enum round-trips for every named role") {
    CHECK(parse_pp_role("define").value() ==    PpRole::Define);
    CHECK(parse_pp_role("undef").value() ==     PpRole::Undef);
    CHECK(parse_pp_role("ifdef").value() ==     PpRole::Ifdef);
    CHECK(parse_pp_role("ifndef").value() ==    PpRole::Ifndef);
    CHECK(parse_pp_role("if").value() ==        PpRole::If);
    CHECK(parse_pp_role("elsif").value() ==     PpRole::Elsif);
    CHECK(parse_pp_role("else").value() ==      PpRole::Else);
    CHECK(parse_pp_role("endif").value() ==     PpRole::Endif);
    CHECK(parse_pp_role("include").value() ==   PpRole::Include);
    CHECK(parse_pp_role("macro_use").value() == PpRole::MacroUse);
    CHECK(parse_pp_role("paste").value() ==     PpRole::Paste);
    CHECK(parse_pp_role("stringify").value() == PpRole::Stringify);
    CHECK(parse_pp_role("text").value() ==      PpRole::Text);
    CHECK_FALSE(parse_pp_role("bogus").has_value());
    CHECK(to_string(PpRole::None) == "");
    CHECK(to_string(PpRole::Define) == "define");
    CHECK(to_string(PpRole::MacroUse) == "macro_use");
}
