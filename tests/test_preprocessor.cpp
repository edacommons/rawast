#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_sv.hpp>
#include <rawast/preprocessor.hpp>

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
