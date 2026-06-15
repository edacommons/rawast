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
    WalkResult walk(StreamReader& sr) override {
        sr.mark();
        const Position start = sr.position();
        auto c = sr.peek();
        if (!c || *c != '\n') {
            sr.reject();
            return tl::unexpected(ParseError{start, "expected newline"});
        }
        sr.get();
        sr.accept();
        return {};
    }
    ValuePtr value() const override { return null_value(); }
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

// Directive alternatives are tried before MACRO_USE / TEXT. Each
// directive's leading literal (`\`define`, `\`ifdef`, …) is the
// discriminator. MACRO_USE matches `\`IDENT` for any other backtick
// reference, but uses negative-lookahead guards to refuse
// `\`endif` / `\`else` — those are structural close tokens for
// IFDEF / ELSE_CLAUSE, not macro references, so MACRO_USE must let
// BODY's repeat fall through at those boundaries.
ITEM: choice {
  <DEFINE>,
  <UNDEF>,
  <IFDEF>,
  <IFNDEF>,
  <IF>,
  <INCLUDE>,
  <MACRO_USE>,
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

// MACRO_USE matches `\`IDENT\n` for any backtick-prefixed identifier
// that isn't itself a structural close token. The `!"\`endif"` and
// `!"\`else"` guards make the negative-lookahead engine path
// explicit — without them the macro-use alternative would greedily
// match `\`endif` as a macro reference to `endif` and break IFDEF
// close detection. Locked in by the engine fix at b887fca.
//
// The trailing `nl` is required so the macro-use line is fully
// consumed; without it, BODY's repeat would stop at the orphan
// newline and the start-rule completion check would fail with
// "unexpected content" — which currently puts the preprocessor in
// pass-through mode and silently emits the raw source.
MACRO_USE: sequence dict {
  !"`endif", !"`else",
  "`":type="macro_use",
  identifier:name=@,
  nl
}:#role="macro_use"

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

// ─── Synthesized-AST helpers ────────────────────────────────────
// The walker is the actual product of the preprocessor — it
// dispatches on a dict's `type` field and standard field names.
// These helpers let tests hand-build that universal AST directly,
// skipping the grammar layer, so we can pin walker behaviour
// independently of whichever .rawast eventually feeds it.

inline ValuePtr str(std::string s) { return make_string(std::move(s)); }

inline ValuePtr arr(std::initializer_list<ValuePtr> items) {
    auto a = make_array();
    auto& v = as_array(a)->data();
    for (auto& it : items) v.push_back(it);
    return a;
}

inline ValuePtr dict(
    std::initializer_list<std::pair<std::string, ValuePtr>> entries) {
    auto d = make_dict();
    auto& m = as_dict(d)->data();
    for (auto& [k, v] : entries) m.emplace(k, v);
    return d;
}

// Top-level synthetic AST: an array of items. The walker iterates
// arrays and dispatches each child on its `type` field.
inline ValuePtr program(std::initializer_list<ValuePtr> items) {
    return arr(items);
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
    CHECK(m->body == "ON");
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

// ─── Synthesized-AST tests ──────────────────────────────────────
// These tests bypass the grammar entirely. They build the universal
// AST that the walker contracts on, hand-craft a matching `source`
// string (the walker uses it to position byte cursors via
// locate_item), and call pp.process_ast() directly. The walker
// itself is the product — these tests pin its contract independent
// of any particular .rawast input grammar.

TEST_CASE("process_ast: synthesized `define registers a macro") {
    Grammar g = make_passthrough_grammar();
    Preprocessor pp(g);
    auto ast = program({
        dict({{"type", str("define")},
              {"name", str("WIDTH")},
              {"body", str("32")}}),
    });
    // Source need only contain the literals locate_item scans for;
    // walker advances cursor past the `define directive line.
    auto out = pp.process_ast(ast, "`define WIDTH 32\n");
    CHECK(out.empty());                          // directive emits nothing
    CHECK(pp.is_defined("WIDTH"));
    REQUIRE(pp.get_macro("WIDTH"));
    CHECK(pp.get_macro("WIDTH")->body == "32");
}

TEST_CASE("process_ast: synthesized `undef removes a macro") {
    Grammar g = make_passthrough_grammar();
    Preprocessor pp(g);
    auto ast = program({
        dict({{"type", str("define")},
              {"name", str("X")},
              {"body", str("1")}}),
        dict({{"type", str("undef")},
              {"name", str("X")}}),
    });
    pp.process_ast(ast, "`define X 1\n`undef X\n");
    CHECK_FALSE(pp.is_defined("X"));
}

TEST_CASE("process_ast: synthesized `ifdef takes body when defined") {
    Grammar g = make_passthrough_grammar();
    Preprocessor pp(g);
    auto ast = program({
        dict({{"type", str("define")},
              {"name", str("A")},
              {"body", str("1")}}),
        dict({{"type", str("ifdef")},
              {"cond", str("A")},
              {"body", arr({
                  dict({{"type", str("text")},
                        {"text", str("YES")}}),
              })}}),
    });
    auto out = pp.process_ast(ast, "`define A 1\n`ifdef A\nYES\n`endif\n");
    CHECK(out.find("YES") != std::string::npos);
}

TEST_CASE("process_ast: synthesized `ifdef drops body when undefined") {
    Grammar g = make_passthrough_grammar();
    Preprocessor pp(g);
    auto ast = program({
        dict({{"type", str("ifdef")},
              {"cond", str("MISSING")},
              {"body", arr({
                  dict({{"type", str("text")},
                        {"text", str("HIDDEN")}}),
              })}}),
    });
    auto out = pp.process_ast(ast, "`ifdef MISSING\nHIDDEN\n`endif\n");
    CHECK(out.find("HIDDEN") == std::string::npos);
}

TEST_CASE("process_ast: undefined_handler fires and supplies expansion") {
    Grammar g = make_passthrough_grammar();
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
    auto ast = program({
        dict({{"type", str("macro_use")},
              {"name", str("GIT_HASH")}}),
    });
    auto out = pp.process_ast(ast, "`GIT_HASH\n");
    CHECK(fired);
    CHECK(out.find("abc123") != std::string::npos);
}

TEST_CASE("process_ast: undefined_handler returning nullopt falls through to policy") {
    Grammar g = make_passthrough_grammar();
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
    auto ast = program({
        dict({{"type", str("macro_use")},
              {"name", str("UNKNOWN")}}),
    });
    auto out = pp.process_ast(ast, "`UNKNOWN\n");
    CHECK(call_count == 1);
    // Empty policy ate the macro site — output should not contain
    // the literal name.
    CHECK(out.find("UNKNOWN") == std::string::npos);
}

TEST_CASE("process_ast: undefined_handler skipped when macro is actually defined") {
    Grammar g = make_passthrough_grammar();
    PpOptions opts;
    bool fired = false;
    opts.undefined_handler = [&](const std::string&,
                                 const std::vector<std::string>&)
        -> std::optional<std::string> {
        fired = true;
        return std::string("FROM_CALLBACK");
    };
    Preprocessor pp(g, std::move(opts));
    auto ast = program({
        dict({{"type", str("define")},
              {"name", str("X")},
              {"body", str("FROM_DEFINE")}}),
        dict({{"type", str("macro_use")},
              {"name", str("X")}}),
    });
    auto out = pp.process_ast(ast, "`define X FROM_DEFINE\n`X\n");
    CHECK_FALSE(fired);
    CHECK(out.find("FROM_DEFINE") != std::string::npos);
    CHECK(out.find("FROM_CALLBACK") == std::string::npos);
}

TEST_CASE("process_ast: undefined_handler receives raw args") {
    Grammar g = make_passthrough_grammar();
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
    auto ast = program({
        dict({{"type", str("macro_use")},
              {"name", str("JOIN")},
              {"args", arr({str("a"), str("b"), str("c")})}}),
    });
    auto out = pp.process_ast(ast, "`JOIN(a,b,c)\n");
    REQUIRE(captured.size() == 3);
    CHECK(captured[0] == "a");
    CHECK(captured[1] == "b");
    CHECK(captured[2] == "c");
    CHECK(out.find("joined") != std::string::npos);
}

TEST_CASE("process_ast: `if takes first branch when expr_eval returns true") {
    Grammar g = make_passthrough_grammar();
    PpOptions opts;
    opts.expr_eval = [](const ValuePtr& cond) -> std::optional<bool> {
        auto s = as_string(cond);
        return s && s->data() == "ALPHA";
    };
    Preprocessor pp(g, std::move(opts));
    auto ast = program({
        dict({{"type", str("if")},
              {"branches", arr({
                  dict({{"cond", str("ALPHA")},
                        {"body", arr({
                            dict({{"type", str("text")},
                                  {"text", str("A_TAKEN")}}),
                        })}}),
              })},
              {"else_branch", arr({
                  dict({{"type", str("text")},
                        {"text", str("ELSE_TAKEN")}}),
              })}}),
    });
    auto out = pp.process_ast(ast,
        "`if ALPHA\nA_TAKEN\n`else\nELSE_TAKEN\n`endif\n");
    CHECK(out.find("A_TAKEN") != std::string::npos);
    CHECK(out.find("ELSE_TAKEN") == std::string::npos);
}

TEST_CASE("process_ast: `if falls to else_branch when no branch true") {
    Grammar g = make_passthrough_grammar();
    PpOptions opts;
    opts.expr_eval = [](const ValuePtr&) -> std::optional<bool> {
        return false;
    };
    Preprocessor pp(g, std::move(opts));
    auto ast = program({
        dict({{"type", str("if")},
              {"branches", arr({
                  dict({{"cond", str("X")},
                        {"body", arr({
                            dict({{"type", str("text")},
                                  {"text", str("X_BODY")}}),
                        })}}),
              })},
              {"else_branch", arr({
                  dict({{"type", str("text")},
                        {"text", str("FALLBACK")}}),
              })}}),
    });
    auto out = pp.process_ast(ast,
        "`if X\nX_BODY\n`else\nFALLBACK\n`endif\n");
    CHECK(out.find("FALLBACK") != std::string::npos);
    CHECK(out.find("X_BODY") == std::string::npos);
}

TEST_CASE("process_ast: `if/`elsif chain picks the first true branch") {
    Grammar g = make_passthrough_grammar();
    PpOptions opts;
    opts.expr_eval = [](const ValuePtr& cond) -> std::optional<bool> {
        auto s = as_string(cond);
        return s && s->data() == "B";
    };
    Preprocessor pp(g, std::move(opts));
    auto ast = program({
        dict({{"type", str("if")},
              {"branches", arr({
                  dict({{"cond", str("A")},
                        {"body", arr({
                            dict({{"type", str("text")},
                                  {"text", str("A_BODY")}}),
                        })}}),
                  dict({{"cond", str("B")},
                        {"body", arr({
                            dict({{"type", str("text")},
                                  {"text", str("B_BODY")}}),
                        })}}),
                  dict({{"cond", str("C")},
                        {"body", arr({
                            dict({{"type", str("text")},
                                  {"text", str("C_BODY")}}),
                        })}}),
              })}}),
    });
    auto out = pp.process_ast(ast,
        "`if A\nA_BODY\n`elsif B\nB_BODY\n`elsif C\nC_BODY\n`endif\n");
    CHECK(out.find("B_BODY") != std::string::npos);
    CHECK(out.find("A_BODY") == std::string::npos);
    CHECK(out.find("C_BODY") == std::string::npos);
}

TEST_CASE("process_ast: `if without expr_eval records warning, skips body") {
    Grammar g = make_passthrough_grammar();
    Preprocessor pp(g);   // no expr_eval set
    auto ast = program({
        dict({{"type", str("if")},
              {"branches", arr({
                  dict({{"cond", str("X")},
                        {"body", arr({
                            dict({{"type", str("text")},
                                  {"text", str("SHOULD_NOT_APPEAR")}}),
                        })}}),
              })}}),
    });
    auto out = pp.process_ast(ast, "`if X\nSHOULD_NOT_APPEAR\n`endif\n");
    CHECK(out.find("SHOULD_NOT_APPEAR") == std::string::npos);
    REQUIRE_FALSE(pp.warnings().empty());
    CHECK(pp.warnings()[0].message.find("expr_eval") != std::string::npos);
}

TEST_CASE("process_ast: expr_eval returning nullopt warns and skips") {
    Grammar g = make_passthrough_grammar();
    PpOptions opts;
    opts.expr_eval = [](const ValuePtr&) -> std::optional<bool> {
        return std::nullopt;
    };
    Preprocessor pp(g, std::move(opts));
    auto ast = program({
        dict({{"type", str("if")},
              {"branches", arr({
                  dict({{"cond", str("UNKNOWN")},
                        {"body", arr({
                            dict({{"type", str("text")},
                                  {"text", str("BODY")}}),
                        })}}),
              })},
              {"else_branch", arr({
                  dict({{"type", str("text")},
                        {"text", str("FALLBACK")}}),
              })}}),
    });
    auto out = pp.process_ast(ast,
        "`if UNKNOWN\nBODY\n`else\nFALLBACK\n`endif\n");
    CHECK(out.find("BODY") == std::string::npos);
    CHECK(out.find("FALLBACK") != std::string::npos);
    REQUIRE_FALSE(pp.warnings().empty());
}

TEST_CASE("process_ast: expr_eval receives structured AST, not raw text") {
    // The cond is a dict tree the grammar would have built —
    // {type:"binop", op:"&&", lhs:..., rhs:...}. Host evaluator
    // walks the tree the same way the preprocessor walker does.
    // This is the whole reason expr_eval takes ValuePtr, not string.
    Grammar g = make_passthrough_grammar();
    PpOptions opts;
    bool saw_dict = false;
    std::string seen_op;
    opts.expr_eval = [&](const ValuePtr& cond) -> std::optional<bool> {
        auto d = as_dict(cond);
        if (!d) return std::nullopt;
        saw_dict = true;
        auto it = d->data().find("op");
        if (it != d->data().end()) {
            if (auto s = as_string(it->second)) seen_op = s->data();
        }
        return true;
    };
    Preprocessor pp(g, std::move(opts));
    auto ast = program({
        dict({{"type", str("if")},
              {"branches", arr({
                  dict({{"cond", dict({
                            {"type", str("binop")},
                            {"op", str("&&")},
                            {"lhs", str("FOO")},
                            {"rhs", str("BAR")}})},
                        {"body", arr({
                            dict({{"type", str("text")},
                                  {"text", str("BODY")}}),
                        })}}),
              })}}),
    });
    auto out = pp.process_ast(ast, "`if FOO && BAR\nBODY\n`endif\n");
    CHECK(saw_dict);
    CHECK(seen_op == "&&");
    CHECK(out.find("BODY") != std::string::npos);
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
