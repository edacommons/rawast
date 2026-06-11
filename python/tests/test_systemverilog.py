"""SystemVerilog grammar integration tests.

Exercises the systemverilog.rawast grammar against real Verilog
constructs: module declarations, ANSI ports, always blocks, expressions,
hex literals, module instantiations. The grammar is loaded via the
meta-grammar (a from_dict round-trip on the .rawast source).

Scope of this first round:
  * Verilog 2001 synthesizable subset — modules through expressions.
  * NOT covered yet: classes, interfaces, packages, SVA, preprocessor.
"""

import pytest
import rawast


@pytest.fixture(scope="module")
def sv_grammar():
    """Compile the SystemVerilog grammar once for the whole module."""
    meta = rawast.Grammar("rawast")
    d = meta.parse_file(rawast.grammar_path("systemverilog.rawast"))
    return rawast.Grammar.from_dict(d)


def unwrap(node):
    """Strip empty-tail passthrough wrappers from an expression node.

    The linear-precedence grammar emits `{lhs: <inner>, tail: [...]}`
    at each binop level when there ARE operators. When there are NONE,
    the `repeat` matches zero times and the binding emits no `tail`
    key at all — leaving just `{lhs: <inner>}`. So a passthrough
    wrapper is either `{lhs: X, tail: []}` OR `{lhs: X}`.

    Strip these recursively until we hit a node with a non-empty tail
    OR any other shape.
    """
    while isinstance(node, dict):
        keys = set(node.keys())
        if keys == {"lhs"}:
            node = node["lhs"]
        elif keys == {"lhs", "tail"} and node["tail"] == []:
            node = node["lhs"]
        else:
            break
    return node


# ─── Module structure ────────────────────────────────────────────────


def test_empty_module(sv_grammar):
    """Smallest valid module — header + endmodule."""
    src = "module foo (); endmodule\n"
    r = sv_grammar.parse_string(src)
    assert r["descriptions"][0]["type"] == "module"
    assert r["descriptions"][0]["name"] == "foo"


def test_module_with_ansi_ports(sv_grammar):
    src = """
    module bar (input wire clk, input wire rst_n, output reg [7:0] q);
    endmodule
    """
    r = sv_grammar.parse_string(src)
    m = r["descriptions"][0]
    assert m["name"] == "bar"
    ports = m["ports"]["ports"]
    assert len(ports) == 3
    assert ports[0]["direction"] == "input"
    assert ports[0]["name"] == "clk"
    assert ports[2]["direction"] == "output"
    assert ports[2]["type_spec"] == "reg"
    # Range [7:0] captured
    assert "range" in ports[2]
    rng = ports[2]["range"]
    assert unwrap(rng["msb"])["type"] == "number"
    assert unwrap(rng["lsb"])["type"] == "number"


def test_module_with_nonansi_ports(sv_grammar):
    """Verilog-95-style port list — just names, declarations below."""
    src = """
    module baz (a, b, c);
      input  wire a;
      input  wire b;
      output wire c;
    endmodule
    """
    r = sv_grammar.parse_string(src)
    m = r["descriptions"][0]
    assert m["ports"]["style"] == "nonansi"
    assert m["ports"]["port_names"] == ["a", "b", "c"]
    # Port declarations show up as port_decl items in the body.
    decls = [i for i in m.get("items", []) if i["type"] == "port_decl"]
    assert len(decls) == 3
    assert decls[0]["direction"] == "input"
    assert decls[2]["direction"] == "output"


# ─── Always blocks + statements ─────────────────────────────────────


def test_counter_with_always_and_reset(sv_grammar):
    """Real-world 8-bit counter with synchronous-clocked / async-reset
    flip-flop. Exercises: always block, edge-triggered sensitivity list,
    if/else, non-blocking assign, sized hex literal, addition."""
    src = """
    module counter (
      input  wire       clk,
      input  wire       rst_n,
      output reg  [7:0] q
    );
      always @(posedge clk or negedge rst_n)
        if (!rst_n) q <= 8'h00;
        else        q <= q + 1;
    endmodule
    """
    r = sv_grammar.parse_string(src)
    m = r["descriptions"][0]
    items = m["items"]
    assert any(i["type"] == "always" for i in items)
    always = [i for i in items if i["type"] == "always"][0]
    assert always["kind"] == "always"
    # Body is an if statement
    body = always["body"]
    assert body["type"] == "if"
    # Then-branch is a non-blocking assign
    assert body["then"]["type"] == "nonblocking_assign"
    # Else-branch is also a non-blocking assign (q <= q + 1)
    assert body["else"]["type"] == "nonblocking_assign"


def test_continuous_assign(sv_grammar):
    src = """
    module andgate (input a, input b, output y);
      assign y = a & b;
    endmodule
    """
    r = sv_grammar.parse_string(src)
    items = r["descriptions"][0]["items"]
    cont = [i for i in items if i["type"] == "cont_assign"][0]
    assignments = cont["assignments"]
    assert assignments[0]["lhs"]["name"] == "y"
    # RHS is `a & b` — bitwise-and. Linear-PEG grammar: at the BAND
    # level, `lhs=a` and `tail=[{op:&, rhs:b}]`. The OUTER passthrough
    # wrappers (LOR/LAND/BOR/BXOR) are stripped by unwrap().
    rhs = unwrap(assignments[0]["rhs"])
    assert rhs["tail"][0]["op"] == "&"
    assert len(rhs["tail"]) == 1


def test_module_instantiation_named_ports(sv_grammar):
    src = """
    module top (input clk, output q);
      counter u0 (.clk(clk), .q(q));
    endmodule
    """
    r = sv_grammar.parse_string(src)
    items = r["descriptions"][0]["items"]
    inst = [i for i in items if i["type"] == "instance"][0]
    assert inst["module_name"] == "counter"
    assert inst["instances"][0]["name"] == "u0"
    bindings = inst["instances"][0]["port_bindings"]
    assert all(b["style"] == "named" for b in bindings)
    assert bindings[0]["port"] == "clk"


# ─── Expressions ─────────────────────────────────────────────────────


def test_expression_arithmetic(sv_grammar):
    """Arithmetic expression `a * 2 + b` — exercises the per-level
    repeat-tail pattern. The IR shape is `{lhs, tail: [{op, rhs}]}` at
    each precedence level, with sub-expressions nested in `lhs`/`rhs`
    when they involve higher-precedence operators."""
    src = """
    module e1 (input [7:0] a, input [7:0] b, output [7:0] y);
      assign y = a * 2 + b;
    endmodule
    """
    r = sv_grammar.parse_string(src)
    cont = [i for i in r["descriptions"][0]["items"]
            if i["type"] == "cont_assign"][0]
    rhs = unwrap(cont["assignments"][0]["rhs"])
    # ADD level (after stripping wrappers): lhs is `a * 2`, tail is `[{op:+, rhs:b}]`
    assert rhs["tail"][0]["op"] == "+"
    # lhs of ADD unwraps to MUL: `a * 2`
    add_lhs = unwrap(rhs["lhs"])
    assert add_lhs["tail"][0]["op"] == "*"


def test_expression_comparison(sv_grammar):
    """Equality with based-number RHS."""
    src = """
    module e2 (input [7:0] x, output flag);
      assign flag = x == 8'hAA;
    endmodule
    """
    r = sv_grammar.parse_string(src)
    cont = [i for i in r["descriptions"][0]["items"]
            if i["type"] == "cont_assign"][0]
    rhs = unwrap(cont["assignments"][0]["rhs"])
    # EQUALITY (after stripping wrappers): lhs=x, tail=[{op:==, rhs: 8'hAA}]
    assert rhs["tail"][0]["op"] == "=="


def test_expression_concatenation(sv_grammar):
    """Concatenation `{a, b}` — verifies the previously-skipped concat
    test now parses linearly (was exponential in nested-expression depth
    before the linear-precedence grammar restructure)."""
    src = """
    module e3 (input [3:0] a, input [3:0] b, output [7:0] y);
      assign y = {a, b};
    endmodule
    """
    r = sv_grammar.parse_string(src)
    cont = [i for i in r["descriptions"][0]["items"]
            if i["type"] == "cont_assign"][0]
    rhs = unwrap(cont["assignments"][0]["rhs"])
    assert rhs["type"] == "concat"
    assert len(rhs["elements"]) == 2


# ─── Number literals ─────────────────────────────────────────────────


def test_sized_hex_literal(sv_grammar):
    src = """
    module n1 (output [7:0] y);
      assign y = 8'hFF;
    endmodule
    """
    r = sv_grammar.parse_string(src)
    cont = [i for i in r["descriptions"][0]["items"]
            if i["type"] == "cont_assign"][0]
    num = unwrap(cont["assignments"][0]["rhs"])
    assert num["type"] == "number"
    n = num["number"]
    assert n["kind"] == "based"
    assert n["size"] == 8
    assert n["base"] == "h"
    assert n["value"] == "FF"


def test_unsized_decimal(sv_grammar):
    src = """
    module n2 (output [31:0] y);
      assign y = 42;
    endmodule
    """
    r = sv_grammar.parse_string(src)
    cont = [i for i in r["descriptions"][0]["items"]
            if i["type"] == "cont_assign"][0]
    n = unwrap(cont["assignments"][0]["rhs"])["number"]
    assert n["kind"] == "integer"
    assert n["value"] == 42


# ─── Comments ───────────────────────────────────────────────────────


def test_module_with_comments(sv_grammar):
    """Both `//` and `/* */` comments are tolerated via the std parsers
    on the ignore list."""
    src = """
    // a 1-bit register
    module r (input clk, /* fancy reset */ input rst_n, output reg q);
      // body
    endmodule
    """
    r = sv_grammar.parse_string(src)
    assert r["descriptions"][0]["name"] == "r"


# ─── Preprocessor — recognition only, no expansion ────────────────────


def test_preprocessor_define(sv_grammar):
    """`define is captured as a directive AST node. The body is raw
    text up to the next un-escaped newline."""
    src = "`define WIDTH 8\nmodule m; endmodule\n"
    r = sv_grammar.parse_string(src)
    d = r["descriptions"][0]
    assert d["type"] == "define"
    assert d["name"] == "WIDTH"
    assert d["body"] == "8"


def test_preprocessor_define_with_continuation(sv_grammar):
    """Backslash-newline line continuation extends the body across
    multiple source lines."""
    src = "`define MULTI first \\\n  second\nmodule m; endmodule\n"
    r = sv_grammar.parse_string(src)
    d = r["descriptions"][0]
    assert d["type"] == "define"
    assert d["name"] == "MULTI"
    # The body preserves the backslash + newline + continuation text.
    assert "first" in d["body"]
    assert "second" in d["body"]


def test_preprocessor_include(sv_grammar):
    src = '`include "definitions.vh"\nmodule m; endmodule\n'
    r = sv_grammar.parse_string(src)
    d = r["descriptions"][0]
    assert d["type"] == "include"
    assert d["file"] == "definitions.vh"


def test_preprocessor_ifdef(sv_grammar):
    """`ifdef body is captured as raw text up to `endif. The host
    evaluates the condition and re-parses the active branch."""
    src = "`ifdef SYNTHESIS\ninitial $display(\"syn\");\n`endif\nmodule m; endmodule\n"
    r = sv_grammar.parse_string(src)
    d = r["descriptions"][0]
    assert d["type"] == "ifdef"
    assert d["cond"] == "SYNTHESIS"
    assert "initial" in d["body"]


def test_preprocessor_undef(sv_grammar):
    src = "`undef WIDTH\nmodule m; endmodule\n"
    r = sv_grammar.parse_string(src)
    d = r["descriptions"][0]
    assert d["type"] == "undef"
    assert d["name"] == "WIDTH"


def test_macro_use_in_expression(sv_grammar):
    """Bare `MACRO at expression position emits a macro_use AST
    node — distinguished from numeric/identifier literals by the
    `type: macro_use` discriminator."""
    src = "module m (output y); assign y = `WIDTH; endmodule\n"
    r = sv_grammar.parse_string(src)
    cont = [i for i in r["descriptions"][0]["items"]
            if i["type"] == "cont_assign"][0]
    rhs = cont["assignments"][0]["rhs"]
    # Unwrap precedence-passthrough layers
    while isinstance(rhs, dict) and set(rhs.keys()) <= {"lhs", "tail"}:
        if rhs.get("tail"):
            break
        rhs = rhs.get("lhs", rhs)
    assert rhs["type"] == "macro_use"
    assert rhs["name"] == "WIDTH"


def test_macro_use_in_number_size(sv_grammar):
    """`WIDTH'd42 is recognized as a based number whose size IS a
    macro_use node. The token form (`MACRO immediately followed
    by `'d42`) is unambiguous: NUMBER_PRIMARY is tried before
    bare MACRO_USE in PRIMARY's Choice."""
    src = "module m (output y); assign y = `WIDTH'd42; endmodule\n"
    r = sv_grammar.parse_string(src)
    cont = [i for i in r["descriptions"][0]["items"]
            if i["type"] == "cont_assign"][0]
    rhs = cont["assignments"][0]["rhs"]
    while isinstance(rhs, dict) and set(rhs.keys()) <= {"lhs", "tail"}:
        if rhs.get("tail"):
            break
        rhs = rhs.get("lhs", rhs)
    assert rhs["type"] == "number"
    n = rhs["number"]
    assert n["kind"] == "based"
    assert n["base"] == "d"
    assert n["value"] == "42"
    assert n["size"]["type"] == "macro_use"
    assert n["size"]["name"] == "WIDTH"


def test_macro_call_with_args(sv_grammar):
    """Function-like macro call `MACRO(a, b) captures args as an
    array of strings, with nested parens balanced via the
    sv_balanced_arg parser."""
    src = "module m (output y); assign y = `MAX(a, b); endmodule\n"
    r = sv_grammar.parse_string(src)
    cont = [i for i in r["descriptions"][0]["items"]
            if i["type"] == "cont_assign"][0]
    rhs = cont["assignments"][0]["rhs"]
    while isinstance(rhs, dict) and set(rhs.keys()) <= {"lhs", "tail"}:
        if rhs.get("tail"):
            break
        rhs = rhs.get("lhs", rhs)
    assert rhs["type"] == "macro_use"
    assert rhs["name"] == "MAX"
    # Args preserve their text exactly as-is — leading space included.
    # The host trims if it cares; we don't lose information.
    assert rhs["args"] == ["a", " b"]


def test_macro_call_with_nested_parens(sv_grammar):
    """Nested parens inside macro args are balanced by the
    depth-tracking sv_balanced_arg parser: `FOO(g(a, b), c)
    yields two args, not three."""
    src = "module m (output y); assign y = `FOO(g(a, b), c); endmodule\n"
    r = sv_grammar.parse_string(src)
    cont = [i for i in r["descriptions"][0]["items"]
            if i["type"] == "cont_assign"][0]
    rhs = cont["assignments"][0]["rhs"]
    while isinstance(rhs, dict) and set(rhs.keys()) <= {"lhs", "tail"}:
        if rhs.get("tail"):
            break
        rhs = rhs.get("lhs", rhs)
    assert rhs["args"] == ["g(a, b)", " c"]


def test_macro_use_as_module_name(sv_grammar):
    """`module `MOD_NAME (...)` — IDENT_OR_MACRO wraps the name slot."""
    src = "module `MOD_NAME (input a, output y); endmodule\n"
    r = sv_grammar.parse_string(src)
    m = r["descriptions"][0]
    assert m["name"]["type"] == "macro_use"
    assert m["name"]["name"] == "MOD_NAME"


def test_macro_use_as_port_name(sv_grammar):
    src = "module m (input `CLK_NAME, output y); endmodule\n"
    r = sv_grammar.parse_string(src)
    port0 = r["descriptions"][0]["ports"]["ports"][0]
    assert port0["name"]["type"] == "macro_use"
    assert port0["name"]["name"] == "CLK_NAME"


def test_macro_use_as_wire_name(sv_grammar):
    src = "module m; wire `SIG_NAME; endmodule\n"
    r = sv_grammar.parse_string(src)
    net = [i for i in r["descriptions"][0]["items"]
           if i["type"] == "net_decl"][0]
    assert net["names"][0]["name"]["type"] == "macro_use"
    assert net["names"][0]["name"]["name"] == "SIG_NAME"


def test_macro_use_as_instance_name(sv_grammar):
    """Both the module type and the instance name can be macros."""
    src = "module m; `MOD_TYPE `U0(.clk(clk)); endmodule\n"
    r = sv_grammar.parse_string(src)
    inst = [i for i in r["descriptions"][0]["items"]
            if i["type"] == "instance"][0]
    assert inst["module_name"]["type"] == "macro_use"
    assert inst["module_name"]["name"] == "MOD_TYPE"
    assert inst["instances"][0]["name"]["type"] == "macro_use"
    assert inst["instances"][0]["name"]["name"] == "U0"


def test_macro_statement(sv_grammar):
    """A bare `MACRO; at statement position parses as a macro_stmt,
    consuming the optional trailing semicolon. Useful for assertion
    macros: `ASSERT_CLOCKED(clk, req, gnt); etc."""
    src = "module m (input clk); always @(*) `MY_ASSERT(clk);\nendmodule\n"
    r = sv_grammar.parse_string(src)
    always = [i for i in r["descriptions"][0]["items"]
              if i["type"] == "always"][0]
    body = always["body"]
    assert body["type"] == "macro_stmt"
    assert body["macro"]["type"] == "macro_use"
    assert body["macro"]["name"] == "MY_ASSERT"


def test_save_produces_output(sv_grammar):
    """save() emits text for a range of SV constructs. Full re-parse
    round-trip isn't asserted here — save currently emits whitespace-
    collapsed output that needs an explicit pretty-print pass before
    re-parsing (strict keywords need separators). Confirming that save
    completes without error for each construct catches dispatch and
    state-machine regressions."""
    sources = [
        # module structure
        "module m; endmodule\n",
        "module m; wire foo; endmodule\n",
        "module m; wire [7:0] bus; endmodule\n",
        "module m (input clk, output [3:0] q); endmodule\n",
        "module m; reg r1, r2; endmodule\n",
        # expressions
        "module m (output y); assign y = 1; endmodule\n",
        "module m (output y); assign y = a + b * c; endmodule\n",
        "module m (output y); assign y = (a); endmodule\n",
        "module m (input s, output y); assign y = s ? 1 : 0; endmodule\n",
        "module m (input a, output y); assign y = ~a; endmodule\n",
        # always blocks + statements
        "module m (input clk, output reg q); always @(posedge clk) q <= 1; endmodule\n",
        "module m (input clk, output reg q); always @(posedge clk or negedge rst) q <= 1; endmodule\n",
        "module m (input clk, output reg q); always @(*) q <= 1; endmodule\n",
        "module m (input clk); always @(posedge clk) begin a <= 1; b <= 2; end endmodule\n",
        "module m (input s, output reg y); always @(*) if (s) y = 1; else y = 0; endmodule\n",
        "module m (input s, output reg y); always @(*) case (s) 1: y = 0; default: y = 1; endcase endmodule\n",
        # system tasks
        "module m; initial $display(\"hi\"); endmodule\n",
        "module m; initial $display(42); endmodule\n",
        "module m; initial $finish; endmodule\n",
        # parameters + instantiation
        "module m; parameter W = 8; endmodule\n",
        "module m; localparam X = 1; endmodule\n",
        "module m; counter u0(.clk(clk), .q(q)); endmodule\n",
        # preprocessor
        "`define WIDTH 8\nmodule m; endmodule\n",
        "`include \"foo.vh\"\nmodule m; endmodule\n",
        "`ifdef X\ninitial $display(\"x\");\n`endif\nmodule m; endmodule\n",
        # macro use in various positions
        "module m (output y); assign y = `WIDTH; endmodule\n",
        "module m (output y); assign y = `WIDTH'd42; endmodule\n",
        "module m (output y); assign y = `MAX(a, b); endmodule\n",
        # full common case
        "`define W 8\nmodule m (input clk, input [`W-1:0] d, output reg [`W-1:0] q); always @(posedge clk) q <= d; endmodule\n",
    ]
    failures = []
    for src in sources:
        ast = sv_grammar.parse_string(src)
        try:
            saved = sv_grammar.save(ast)
            assert saved, f"save returned empty for {src!r}"
            assert len(saved) > 0
        except Exception as e:
            failures.append((src, str(e)[:60]))
    if failures:
        msg = "\n".join(f"  {src!r}: {err}" for src, err in failures)
        raise AssertionError(f"{len(failures)} save failures:\n{msg}")


def test_define_then_module_use(sv_grammar):
    """Real-world common case: `define at top-level, then a module
    that uses the macro in a port range and an assignment."""
    src = (
        "`define WIDTH 8\n"
        "module m (output [`WIDTH-1:0] y);\n"
        "  assign y = `WIDTH;\n"
        "endmodule\n"
    )
    r = sv_grammar.parse_string(src)
    # First description: the directive.
    d0 = r["descriptions"][0]
    assert d0["type"] == "define"
    assert d0["name"] == "WIDTH"
    # Second description: the module.
    d1 = r["descriptions"][1]
    assert d1["type"] == "module"
    # The port range MSB is an EXPR containing a macro_use.
    port = d1["ports"]["ports"][0]
    range_msb_str = str(port["range"])
    assert "macro_use" in range_msb_str
    assert "WIDTH" in range_msb_str
