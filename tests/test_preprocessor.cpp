#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
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

// Match exactly one `\n` byte. Used as the line terminator in the
// minimal preprocessor grammar below — the grammar loader doesn't
// unescape `\n` inside Key literals (only `tail="..."` attributes get
// unescape), so a plain `"\n"` key would try to match the literal
// two-character sequence `\` `n`. Registering a tiny test-local
// `nl` parser sidesteps that without making the demo grammar depend
// on any non-std terminal.
class TestNewlineParser final : public Parser {
public:
    TestNewlineParser() : Parser("nl") {}
    ParseResult parse(StreamReader& sr) override {
        sr.mark();
        const Position start = sr.position();
        auto c = sr.peek();
        if (!c || *c != '\n') {
            sr.reject();
            return tl::unexpected(ParseError{start, "expected newline"});
        }
        sr.get();
        sr.accept();
        return null_value();
    }
    SaveResult unparse(const Value& /*v*/) const override {
        return std::string{"\n"};
    }
};

// Minimal preprocessor grammar for testing the generic walker
// without dragging in a language-specific grammar. Backtick-prefixed
// directives mirror the literal-prefix shape the walker's
// `locate_item` helper looks for (the walker is currently coupled to
// backtick syntax — that coupling can be untied once callbacks land).
//
// Recognised directives: `define, `undef, `ifdef, `ifndef, `if,
// `else, `elsif, `endif, `include, `macro_use (via `IDENT). Lines
// without a leading backtick parse as PP_TEXT.
//
// Uses only the `std` parser group — no SV-specific terminals — so
// the harness has no external grammar-file dependency and the
// callbacks/tests targeting it can stay focused on the walker
// mechanism itself.
constexpr const char* MINI_PREPROCESSOR_GRAMMAR = R"(
use: std

start: <DOC>

DOC ignore linespace: sequence array {
  repeat <ITEM>
}

// Directive alternatives are tried before TEXT. Each directive's
// leading literal (`\`define`, `\`ifdef`, …) is the discriminator;
// `\`endif` and `\`else` deliberately have no matching ITEM, so
// BODY's repeat stops cleanly at the close of a conditional block.
//
// MACRO_USE (`\`IDENT`) is intentionally omitted from the minimal
// harness because it would otherwise consume `\`endif` as a macro
// reference and break IFDEF's close detection — `\`endif` parses as
// `\`` + identifier `endif`. The grammar-level fix is `!"\`endif"` in
// MACRO_USE, but the engine's negative-lookahead doesn't currently
// fire on direct-popped Key frames (see grammar.cpp:1336-1360 vs.
// the is_negative check in advance_after_child). Adding MACRO_USE
// belongs with that engine fix; not blocking task #182.
ITEM: choice {
  <DEFINE>,
  <UNDEF>,
  <IFDEF>,
  <IFNDEF>,
  <IF>,
  <INCLUDE>,
  <TEXT>
}

// Body is a single identifier — keeps the minimal harness focused
// on the walker mechanism. The SV preprocessor grammar models richer
// bodies via `sv_line_text`; that's a feature of the SV terminals,
// not the walker.
DEFINE: sequence dict {
  "`define":type="define",
  identifier:name=@,
  identifier:body=@,
  nl
}:#role="define"

UNDEF: sequence dict {
  "`undef":type="undef",
  identifier:name=@,
  nl
}:#role="undef"

IFDEF: sequence dict {
  "`ifdef":type="ifdef",
  identifier:cond=@, nl,
  <BODY>:body=@,
  ?<ELSE_CLAUSE>:else_branch=@,
  "`endif", nl
}:#role="ifdef"

IFNDEF: sequence dict {
  "`ifndef":type="ifndef",
  identifier:cond=@, nl,
  <BODY>:body=@,
  ?<ELSE_CLAUSE>:else_branch=@,
  "`endif", nl
}:#role="ifndef"

IF: sequence dict {
  "`if":type="if",
  identifier:cond=@, nl,
  <BODY>:body=@,
  ?<ELSE_CLAUSE>:else_branch=@,
  "`endif", nl
}:#role="if"

ELSE_CLAUSE: sequence dict {
  "`else", nl,
  <BODY>:body=@
}

BODY: sequence array {
  repeat <ITEM>
}

INCLUDE: sequence dict {
  "`include":type="include",
  string:path=@, nl
}:#role="include"

// TEXT captures one identifier-shaped token per line. No leading
// `!"\`"` guard is needed: `identifier` requires a letter/underscore
// start, so a `\`endif` line (or any backtick-prefixed line) cleanly
// fails to match here. Arbitrary free-text passthrough belongs in a
// more capable preprocessor grammar — this minimal harness is about
// driving the walker, not modelling real source.
TEXT: sequence dict {
  identifier:text=@:type="text",
  nl
}:#role="text"
)";

Grammar make_mini_preprocessor() {
    register_std_parser_group();
    Grammar g;
    // Register the test-local `nl` newline parser before loading so
    // the grammar's `nl` references resolve.
    g.register_parser(std::make_unique<TestNewlineParser>());
    auto r = load_rawast_grammar_from_string(g, MINI_PREPROCESSOR_GRAMMAR);
    REQUIRE_MESSAGE(r, "loading minimal preprocessor grammar failed: "
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
    CHECK(m->body == "BAR");
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
