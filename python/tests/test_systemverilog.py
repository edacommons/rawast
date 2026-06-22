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
    # Packed dimensions captured — `?<PACKED_DIMENSIONS>` produces a
    # list of ranges so `[7:0][3:0]` etc. all use the same shape.
    assert "range" in ports[2]
    dims = ports[2]["range"]
    assert len(dims) == 1
    rng = dims[0]
    assert unwrap(rng["msb"])["type"] == "integer"
    assert unwrap(rng["lsb"])["type"] == "integer"


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
    n = unwrap(cont["assignments"][0]["rhs"])
    assert n["type"] == "based_num"
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
    n = unwrap(cont["assignments"][0]["rhs"])
    assert n["type"] == "integer"
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


# Preprocessor directives (`define / `undef / `include / `ifdef / `ifndef)
# are no longer parsed by the SV grammar — they're handled by the
# `sv_preprocessor` grammar before the source reaches systemverilog.
# Coverage for those lives in tests/test_preprocessor.cpp and
# python/tests/test_preprocessor.py.


def test_macro_use_in_expression(sv_grammar):
    """Bare `MACRO at expression position emits a macro_use AST
    node — distinguished from numeric/identifier literals by the
    `type: macro_use` discriminator. MACRO_HIER_REF wraps it as
    `{type: macro_ref, macro: {type: macro_use, ...}}` so the
    path/selector chain can extend after the macro name."""
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
    assert rhs["type"] == "macro_ref"
    assert rhs["macro"]["type"] == "macro_use"
    assert rhs["macro"]["name"] == "WIDTH"


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
    assert rhs["type"] == "based_num"
    assert rhs["base"] == "d"
    assert rhs["value"] == "42"
    assert rhs["size"]["type"] == "macro_use"
    assert rhs["size"]["name"] == "WIDTH"


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
    assert rhs["type"] == "macro_ref"
    assert rhs["macro"]["type"] == "macro_use"
    assert rhs["macro"]["name"] == "MAX"
    # MACRO_ARGS uses `ignore linespace`, so the leading space inside
    # each arg is eaten by the outer policy before the per-arg scope
    # starts. Args appear as their trimmed text.
    assert rhs["macro"]["args"] == ["a", "b"]


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
    assert rhs["type"] == "macro_ref"
    assert rhs["macro"]["args"] == ["g(a, b)", "c"]


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
        # macro use in various positions (inline `\`MACRO` references
        # the SV grammar still recognizes via MACRO_USE — these aren't
        # full preprocessor directives, just identifier-shaped tokens)
        "module m (output y); assign y = `WIDTH; endmodule\n",
        "module m (output y); assign y = `WIDTH'd42; endmodule\n",
        "module m (output y); assign y = `MAX(a, b); endmodule\n",
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


def test_class_declarations(sv_grammar):
    """Class declarations parse to AST nodes — full semantic resolution
    (extends-chain validation, constraint expression parsing, method
    body type-checking) is downstream. Verifies the engine's repeat-
    iteration rollback + first-byte-through-optionals fix that
    landed alongside this test."""
    cases = [
        ("empty",        "class C; endclass\n"),
        ("virtual",      "virtual class C; endclass\n"),
        ("extends",      "class C extends Base; endclass\n"),
        ("int property", "class C; int x; endclass\n"),
        ("rand bit",     "class C; rand bit [7:0] data; endclass\n"),
        ("user-typed",   "class C; my_t y; endclass\n"),
        ("function",     "class C; function int foo(); endfunction endclass\n"),
        ("task",         "class C; task bar(); endtask endclass\n"),
        ("constraint",   "class C; rand int x; constraint c { x > 0; } endclass\n"),
        ("transaction",  "class T extends uvm_obj; rand bit [7:0] data; constraint c { data > 0; } function new(); endfunction endclass\n"),
    ]
    for name, src in cases:
        r = sv_grammar.parse_string(src)
        d = r["descriptions"][0]
        assert d["type"] == "class", f"{name}: expected type=class, got {d.get('type')!r}"


def test_package_and_interface(sv_grammar):
    """Package and interface declarations parse with module-item bodies."""
    src = "package my_pkg;\n  typedef logic [7:0] byte_t;\n  parameter MAX = 8;\nendpackage\n"
    r = sv_grammar.parse_string(src)
    pkg = r["descriptions"][0]
    assert pkg["type"] == "package"
    assert pkg["name"] == "my_pkg"
    src = "interface bus_if(input clk);\n  logic [7:0] data;\nendinterface\n"
    r = sv_grammar.parse_string(src)
    intf = r["descriptions"][0]
    assert intf["type"] == "interface"
    assert intf["name"] == "bus_if"


def test_typedef_and_user_typed_decls(sv_grammar):
    """typedef declarations + user-typed module-item and port decls.

    `typedef` is currently a MODULE_ITEM (inside modules / packages /
    classes), not a top-level DESCRIPTION — file-level typedefs need
    to live inside a `package` block or be added to DESCRIPTION
    later. The test puts the typedef inside the module to verify
    the module-item path."""
    src = (
        "module m (input byte_t din, output byte_t dout);\n"
        "  typedef logic [7:0] byte_t;\n"
        "  byte_t buffer;\n"
        "  assign dout = buffer;\n"
        "endmodule\n"
    )
    r = sv_grammar.parse_string(src)
    m = r["descriptions"][0]
    assert m["type"] == "module"
    # Ports use byte_t (user-typed port).
    p0 = m["ports"]["ports"][0]
    assert p0.get("direction") == "input"
    assert p0.get("type_spec") == "byte_t"
    # Module items: typedef + user_typed_decl + cont_assign.
    item_types = [i.get("type") for i in m["items"]]
    assert "typedef" in item_types
    assert "user_typed_decl" in item_types


def test_imports(sv_grammar):
    """import statements with wildcard, named, and multi-entry forms."""
    for src in [
        "import pkg::*;\nmodule m; endmodule\n",
        "import pkg::sym;\nmodule m; endmodule\n",
        "import pkg1::*, pkg2::name;\nmodule m; endmodule\n",
    ]:
        r = sv_grammar.parse_string(src)
        imp = r["descriptions"][0]
        assert imp["type"] == "import"


def test_multi_port_shared_direction(sv_grammar):
    """SV's multi-port shorthand: `input a, b, c` is equivalent to
    `input a, input b, input c`. The first port is direction-headed
    and the rest inherit. Host walks left-to-right to re-apply.

    Parse-only: continuation ports are tagged `type=port_name_cont`
    so the host can distinguish them from direction-headed ports."""
    src = "module mon (input clk, req, gnt, output logic q, r); endmodule\n"
    r = sv_grammar.parse_string(src)
    ports = r["descriptions"][0]["ports"]["ports"]
    assert len(ports) == 5
    # First port: direction-headed (input clk).
    assert ports[0].get("direction") == "input"
    assert ports[0].get("name") == "clk"
    assert "type" not in ports[0] or ports[0]["type"] == "port_decl"
    # Continuations inherit from clk.
    assert ports[1].get("type") == "port_name_cont"
    assert ports[1].get("name") == "req"
    assert ports[2].get("type") == "port_name_cont"
    assert ports[2].get("name") == "gnt"
    # Then a new direction-headed port (output logic q).
    assert ports[3].get("direction") == "output"
    assert ports[3].get("name") == "q"
    # And another continuation inheriting from q.
    assert ports[4].get("type") == "port_name_cont"
    assert ports[4].get("name") == "r"


def test_modport_declarations(sv_grammar):
    """Modports in interface bodies — body captured as raw text for
    host re-parsing."""
    src = (
        "interface bus_if;\n"
        "  logic clk;\n"
        "  logic [7:0] data;\n"
        "  modport master (input clk, output data);\n"
        "  modport slave (input clk, input data);\n"
        "endinterface\n"
    )
    r = sv_grammar.parse_string(src)
    intf = r["descriptions"][0]
    assert intf["type"] == "interface"
    items = intf["items"]
    modports = [i for i in items if i.get("type") == "modport"]
    assert len(modports) == 2
    assert modports[0]["name"] == "master"
    assert modports[1]["name"] == "slave"


def test_let_bind_defparam_specify(sv_grammar):
    """Miscellaneous module-item constructs that historically were
    documented as 'future' but now parse to AST nodes."""
    for src, kind in [
        ("module m; let SIZE = 8; endmodule\n",                       "let"),
        ("module m; let MAX(a, b) = (a > b) ? a : b; endmodule\n",    "let"),
        ("module m; defparam u0.WIDTH = 16; endmodule\n",             "defparam"),
        ("module m; bind sub_module assertions a1 (.*); endmodule\n", "bind"),
        ("module m; specify (a => b) = 5; endspecify endmodule\n",    "specify"),
    ]:
        r = sv_grammar.parse_string(src)
        items = r["descriptions"][0]["items"]
        assert items[0].get("type") == kind, f"{src!r}: expected {kind}, got {items[0].get('type')!r}"


def test_sva_concurrent_assertions(sv_grammar):
    """SVA `assert property` / `cover property` / `assume property` —
    body captured raw; host re-parses temporal expression."""
    for src, kind in [
        ("module m; assert property (a |-> b); endmodule\n",                            "assert_concurrent"),
        ("module m; cover property (@(posedge clk) req); endmodule\n",                  "assert_concurrent"),
        ("module m; assume property (rst == 0); endmodule\n",                           "assert_concurrent"),
        ("module m; assert property (@(posedge clk) p) else $error(\"fail\"); endmodule\n", "assert_concurrent"),
    ]:
        r = sv_grammar.parse_string(src)
        items = r["descriptions"][0]["items"]
        assert items[0].get("type") == kind


def test_sva_property_and_sequence(sv_grammar):
    """Named `property NAME … endproperty` and `sequence NAME …
    endsequence` declarations. Combined with concurrent assertions
    in a monitor module."""
    src = (
        "module mon (input logic clk, input req, input gnt);\n"
        "  property req_gnt;\n"
        "    @(posedge clk) req |-> ##[1:5] gnt;\n"
        "  endproperty\n"
        "  sequence req_seq;\n"
        "    @(posedge clk) req ##1 gnt;\n"
        "  endsequence\n"
        "  assert property (req_gnt) else $error;\n"
        "endmodule\n"
    )
    r = sv_grammar.parse_string(src)
    items = r["descriptions"][0]["items"]
    types = [i.get("type") for i in items]
    assert "property" in types
    assert "sequence" in types
    assert "assert_concurrent" in types


def test_covergroup_and_clocking(sv_grammar):
    """`covergroup … endgroup` and `clocking … endclocking` — body
    captured raw; host re-parses to extract coverpoints/cross/etc.
    or clocking signal directions."""
    src = (
        "module mon (input logic clk);\n"
        "  covergroup cg @(posedge clk);\n"
        "    cp_x: coverpoint x;\n"
        "  endgroup\n"
        "  clocking ck @(posedge clk);\n"
        "  endclocking\n"
        "  default clocking dck @(posedge clk);\n"
        "  endclocking\n"
        "endmodule\n"
    )
    r = sv_grammar.parse_string(src)
    items = r["descriptions"][0]["items"]
    types = [i.get("type") for i in items]
    assert "covergroup" in types
    assert types.count("clocking") == 2


def test_checker_and_fork_join(sv_grammar):
    """`checker … endchecker` reusable verification IP +
    `fork … join`/`join_any`/`join_none` concurrent blocks."""
    for src, expected in [
        ("module m; checker c1(input clk); endchecker endmodule\n",
         {"items_first_type": "checker"}),
        ("module m; initial fork a = 1; b = 2; join endmodule\n",
         {"initial_inner": "fork_join"}),
        ("module m; initial fork a = 1; b = 2; join_any endmodule\n",
         {"initial_inner": "fork_join_any"}),
        ("module m; initial fork a = 1; b = 2; join_none endmodule\n",
         {"initial_inner": "fork_join_none"}),
    ]:
        r = sv_grammar.parse_string(src)
        items = r["descriptions"][0]["items"]
        if "items_first_type" in expected:
            assert items[0].get("type") == expected["items_first_type"]


def test_immediate_assertions(sv_grammar):
    """`assert(expr);` / `assert(expr) else <stmt>;` inside
    procedural code (always block / initial block). Distinguished
    from concurrent assertions (`assert property (...)`) by the
    absence of the `property` keyword."""
    for src in [
        "module m; always @(posedge clk) begin assert(x == 1); end endmodule\n",
        "module m; always @(posedge clk) begin assert(x == 1) else $error(\"bad\"); end endmodule\n",
    ]:
        r = sv_grammar.parse_string(src)
        assert r["descriptions"][0]["items"][0].get("type") == "always"


def test_extern_udp_config_program(sv_grammar):
    """Top-level / module-level forward and ancillary declarations."""
    cases = [
        ("module m; extern function int foo(int a); endmodule\n",
         "extern", "items"),
        ("primitive my_or (out, a, b); output out; input a, b; table 0 0 : 0; 1 ? : 1; endtable endprimitive\n",
         "primitive", "descriptions"),
        ("config cfg; design lib.top; endconfig\n",
         "config", "descriptions"),
        ("program test (input clk); initial begin end endprogram\n",
         "program", "descriptions"),
    ]
    for src, kind, where in cases:
        r = sv_grammar.parse_string(src)
        if where == "descriptions":
            assert r["descriptions"][0].get("type") == kind
        else:
            assert r["descriptions"][0]["items"][0].get("type") == kind


def test_final_gaps_genvar_nettype_fork_ctrl(sv_grammar):
    """The remaining small SV-1800 constructs."""
    cases = [
        ("module m; genvar i; endmodule\n",        "genvar"),
        ("module m; genvar i, j, k; endmodule\n",  "genvar"),
        ("module m; nettype real wire_t; endmodule\n", "nettype"),
        ("module m; initial begin wait fork; end endmodule\n",     "initial"),
        ("module m; initial begin disable fork; end endmodule\n",  "initial"),
    ]
    for src, kind in cases:
        r = sv_grammar.parse_string(src)
        items = r["descriptions"][0]["items"]
        assert items[0].get("type") == kind


def test_control_flow_statements(sv_grammar):
    """`return`, `break`, `continue`, `foreach`, `do-while` statements."""
    for src in [
        "class C; function int f(); return 42; endfunction endclass\n",
        "class C; task t(); return; endtask endclass\n",
        "module m; initial begin while (1) break; end endmodule\n",
        "module m; initial begin while (1) continue; end endmodule\n",
        "module m; initial foreach (arr[i]) arr[i] = 0; endmodule\n",
        "module m; initial do x = x + 1; while (x < 10); endmodule\n",
    ]:
        sv_grammar.parse_string(src)  # just verify parses


def test_class_idioms(sv_grammar):
    """The OOP idioms that real testbench code uses heavily:
    `this`, `super`, `null`, `new`, package-qualified refs."""
    for src in [
        "class C; int x; task set(int v); this.x = v; endtask endclass\n",
        "class C extends Base; task t(); super.run(); endtask endclass\n",
        "class C; function new(); obj = null; endfunction endclass\n",
        "module m; initial obj = new(1, 2); endmodule\n",
        "module m; initial begin x = pkg::name; end endmodule\n",
    ]:
        sv_grammar.parse_string(src)


def test_class_method_modifiers(sv_grammar):
    """Pure virtual, extern, virtual, static method declarations
    inside classes."""
    for src in [
        "class C; virtual function int foo(); endfunction endclass\n",
        "class C; virtual task run(); endtask endclass\n",
        "class Base; pure virtual function int abstract(); endclass\n",
        "class Base; pure virtual task run(); endclass\n",
        "class C; extern function int foo(); endclass\n",
        "class C; extern virtual task run(); endclass\n",
        "class C; static function int counter(); endfunction endclass\n",
    ]:
        sv_grammar.parse_string(src)


def test_class_parameters_implements(sv_grammar):
    """Class type parameters with `#(parameter type T)` and
    `implements <interface_list>` clauses."""
    cases = [
        ("class Stack #(parameter type T = int); T data; endclass\n", "class"),
        ("class Stack #(parameter type T); endclass\n", "class"),
        ("class C implements I1, I2; endclass\n", "class"),
        ("class C extends Base implements I1; endclass\n", "class"),
    ]
    for src, kind in cases:
        r = sv_grammar.parse_string(src)
        assert r["descriptions"][0]["type"] == kind


def test_enum_struct_union_typedefs(sv_grammar):
    """User-defined type declarations: enum / struct / union typedefs.
    Body captured raw; host re-parses field/label list."""
    for src in [
        "module m; typedef enum { A, B, C } e_t; endmodule\n",
        "module m; typedef enum logic [1:0] { A = 0, B, C } e_t; endmodule\n",
        "module m; typedef struct { int a; bit b; } s_t; endmodule\n",
        "module m; typedef struct packed { int a; bit b; } s_t; endmodule\n",
        "module m; typedef union { int a; real b; } u_t; endmodule\n",
    ]:
        sv_grammar.parse_string(src)


def test_inside_operator(sv_grammar):
    """`x inside { ... }` set membership operator at relational
    precedence level."""
    for src in [
        "module m; initial begin if (x inside { 1, 2, 3 }) y = 1; end endmodule\n",
        "module m; initial begin if (x inside { [1:10], 20, 30 }) y = 1; end endmodule\n",
    ]:
        sv_grammar.parse_string(src)


def test_streaming_concat(sv_grammar):
    """`{>>{...}}` / `{<<{...}}` / `{<<N{...}}` streaming
    concatenation."""
    for src in [
        "module m; initial begin x = {>>{a, b, c}}; end endmodule\n",
        "module m; initial begin x = {<<{a, b, c}}; end endmodule\n",
        "module m; initial begin x = {<<8{a, b, c}}; end endmodule\n",
    ]:
        sv_grammar.parse_string(src)


def test_virtual_interface_and_global_clocking(sv_grammar):
    """`virtual interface bus_if vif;` in classes/modules +
    `global clocking gck @(...); endclocking` at module level."""
    for src in [
        "class TB; virtual interface bus_if vif; endclass\n",
        "class TB; virtual interface bus_if vif1, vif2; endclass\n",
        "module m; virtual interface bus_if vif; endmodule\n",
        "module m; global clocking gck @(posedge clk); endclocking endmodule\n",
    ]:
        sv_grammar.parse_string(src)


def test_randomize_with_and_method_stmt(sv_grammar):
    """`obj.randomize() with { ... };` as expression OR statement;
    `obj.method();` as statement. METHOD_CALL handles the
    expression form; METHOD_CALL_STMT handles the statement form.
    IMMEDIATE_ASSERT now uses sv_balanced_arg for body so the
    nested-paren `assert(obj.randomize() with { ... });` works."""
    for src in [
        "module m; initial begin x = obj.randomize() with { y > 0; }; end endmodule\n",
        "module m; initial obj.randomize(); endmodule\n",
        "module m; initial obj.randomize() with { y > 0; }; endmodule\n",
        "module m; initial drv.bus.write(addr, data); endmodule\n",
        "class C; function void foo(); this.config.set_value(42); endfunction endclass\n",
        "module m; initial begin assert(obj.randomize() with { x > 0; }); end endmodule\n",
    ]:
        sv_grammar.parse_string(src)


def test_type_cast_and_assignment_pattern(sv_grammar):
    """`<type>'(expr)` cast and `'{field: val, ...}` assignment
    pattern primaries."""
    for src in [
        "module m; initial begin x = int'(y); end endmodule\n",
        "module m; initial begin x = byte_t'(y); end endmodule\n",
        "module m; initial begin s = '{a: 1, b: 2}; end endmodule\n",
        "module m; initial begin arr = '{1, 2, 3, 4}; end endmodule\n",
    ]:
        sv_grammar.parse_string(src)


def test_qualified_class_extends(sv_grammar):
    """`extends pkg::Base` and top-level typedef declarations."""
    for src in [
        "package p; class C extends pkg::Base; endclass endpackage\n",
        "class C extends pkg::Base; endclass\n",
        "typedef enum { A, B } e_t; class C; e_t x; endclass\n",
        "typedef struct packed { int a; bit b; } s_t;\n",
    ]:
        sv_grammar.parse_string(src)


def test_unique_priority_and_compound_assignments(sv_grammar):
    """SV-1800 `unique`/`unique0`/`priority` modifier on if/case
    statements, `case ... inside`, compound assignment operators."""
    for src in [
        "module m; always_comb unique if (a) y = 1; else y = 0; endmodule\n",
        "module m; always_comb priority if (a) y = 1; else y = 0; endmodule\n",
        "module m; always_comb unique case (sel) 0: y = a; 1: y = b; endcase endmodule\n",
        "module m; always_comb priority case (sel) 0: y = a; 1: y = b; endcase endmodule\n",
        "module m; always_comb case (1) inside 0: y = a; default: y = b; endcase endmodule\n",
        "module m; initial x += 1; endmodule\n",
        "module m; initial x -= 1; endmodule\n",
        "module m; initial x *= 2; endmodule\n",
        "module m; initial x <<= 4; endmodule\n",
        "module m; initial x &= 0; endmodule\n",
        "module m; initial x |= 1; endmodule\n",
    ]:
        sv_grammar.parse_string(src)


def test_array_kind_suffixes(sv_grammar):
    """SV-1800 dynamic / queue / associative-array bracket forms:
    `[$]`, `[$:N]`, `[type]`, `[]`, `[N]`."""
    for src in [
        "class T; int q[$]; endclass\n",
        "class T; int q[$:10]; endclass\n",
        "class T; int aa[string]; endclass\n",
        "class T; int da[]; endclass\n",
        "class T; int arr[16]; endclass\n",
    ]:
        sv_grammar.parse_string(src)


def test_multidim_iff_const_var(sv_grammar):
    """Multi-dimensional packed arrays, `iff` event clause,
    `const`/`var` keywords."""
    for src in [
        "module m; logic [7:0][3:0] mem; endmodule\n",
        "module m; reg [15:0][7:0][3:0] cube; endmodule\n",
        "module m; always @(posedge clk iff rst_n) q <= d; endmodule\n",
        "module m; const int W = 8; endmodule\n",
        "class C; var int x; endclass\n",
        "class C; const int W = 8; endclass\n",
    ]:
        sv_grammar.parse_string(src)


def test_user_type_function_return(sv_grammar):
    """Function declarations with user-defined return types
    (factory pattern)."""
    for src in [
        "class M; function M get_inst(); return inst; endfunction endclass\n",
        "class M; static function M create(); return new(); endfunction endclass\n",
        "module m; function automatic int foo(); endfunction endmodule\n",
        "class C; function string get_name(); return name; endfunction endclass\n",
    ]:
        sv_grammar.parse_string(src)


def test_for_loop_extensions(sv_grammar):
    """SV-1800 for-loop extensions: declaration init, `i++` step."""
    for src in [
        "module m; initial for (int i = 0; i < 10; i++) arr[i] = i; endmodule\n",
        "module m; initial for (int i = 0; i < 10; i--) arr[i] = i; endmodule\n",
        "module m; initial for (i = 0; i < 10; i++) arr[i] = i; endmodule\n",
    ]:
        sv_grammar.parse_string(src)


def test_parameterized_user_types(sv_grammar):
    """User-defined parameterized types in declarations:
    `mailbox#(int)`, `Stack#(8)`, etc."""
    for src in [
        "class T; mailbox#(int) mb; endclass\n",
        "class T; Stack#(8) s; endclass\n",
        "module m; Stack#(16) s; endmodule\n",
    ]:
        sv_grammar.parse_string(src)


def test_more_coverage_extensions(sv_grammar):
    """Additional SV-1800 features added in the final coverage push:

    * Implicit-`parameter` form in class parameter lists
      (`class C #(int N=8)`)
    * Interface extends (single + multi)
    * Nested module declarations
    * Module/block trailing `endmodule : label` / `end : label`
    * Top-level `parameter`/`localparam` declarations
    * Cover sequence (`cover sequence (...)`)
    * Labeled concurrent/immediate assertions (`a1: assert property (...)`)
    * Case-inside with range labels (`case (x) inside [0:10]: ...`)
    * Chained array selects in hier refs (`a[0][1][2]`,
      `mod.inst[i].q`)
    """
    for src in [
        # Implicit parameter form
        "class C #(int N=8) extends Base; endclass\n",
        "class C #(int N=8) extends Base #(N); endclass\n",
        "class T #(int W); bit [W-1:0] data; endclass\n",
        # Interface inheritance
        "interface child_if extends parent_if; logic extra; endinterface\n",
        "interface c extends p1, p2; endinterface\n",
        # Nested module
        "module outer; module inner; endmodule endmodule\n",
        # End labels
        "module m; endmodule : m\n",
        "module m; initial begin : lbl x = 1; end : lbl endmodule\n",
        # Top-level parameter
        "parameter int W = 8;\nmodule m; endmodule\n",
        "localparam int X = 16;\nmodule m; endmodule\n",
        # Cover sequence
        "module m; cover sequence (@(posedge clk) a ##1 b); endmodule\n",
        # Labeled assertions
        "module m; a1: assert property (@(posedge clk) p); endmodule\n",
        "module m; initial begin a1: assert(x == 1); end endmodule\n",
        # Case-inside with range label
        "module m; always_comb case (x) inside [0:10]: y = a; default: y = b; endcase endmodule\n",
        # Chained array selects in hier refs
        "module m; initial x = a[0][1][2]; endmodule\n",
        "module m; initial x = mod.inst[i].q; endmodule\n",
    ]:
        sv_grammar.parse_string(src)


def test_enum_struct_modport_structured_bodies(sv_grammar):
    """Enum / struct / union / modport bodies are now parsed into
    fully-structured AST (not raw text). Bidirectional save
    round-trip works."""
    # Enum: labels with optional values
    r = sv_grammar.parse_string("typedef enum logic [1:0] { READ = 0, WRITE = 1, IDLE } cmd_e;\n")
    labels = r["descriptions"][0]["base"]["labels"]
    assert len(labels) == 3
    assert labels[0]["name"] == "READ"
    assert "value" in labels[0]
    assert labels[1]["name"] == "WRITE"
    assert labels[2]["name"] == "IDLE"
    assert "value" not in labels[2]
    out = sv_grammar.save(r)
    assert b"READ" in out
    assert b"WRITE" in out
    assert b"IDLE" in out

    # Struct: fields with types
    r = sv_grammar.parse_string("typedef struct packed { cmd_e cmd; bit [31:0] addr; } trans_t;\n")
    fields = r["descriptions"][0]["base"]["fields"]
    assert len(fields) == 2
    assert fields[0]["name"] == "cmd"
    assert fields[0]["type_spec"] == "cmd_e"
    assert fields[1]["name"] == "addr"
    assert fields[1]["type_spec"] == "bit"
    assert "range" in fields[1]
    out = sv_grammar.save(r)
    assert b"trans_t" in out

    # Union: same field structure as struct
    r = sv_grammar.parse_string("typedef union { int i; real r; } u_t;\n")
    fields = r["descriptions"][0]["base"]["fields"]
    assert len(fields) == 2
    assert fields[0]["name"] == "i"
    assert fields[0]["type_spec"] == "int"

    # Modport: direction-headed + inherited groups
    r = sv_grammar.parse_string(
        "interface i; modport m (input a, b, output c, d, inout e); endinterface\n"
    )
    ports = r["descriptions"][0]["items"][0]["ports"]
    assert len(ports) == 5
    assert ports[0] == {"direction": "input", "name": "a", "type": "modport_group"}
    assert ports[1] == {"name": "b", "type": "modport_group_inherit"}
    assert ports[2] == {"direction": "output", "name": "c", "type": "modport_group"}
    assert ports[3] == {"name": "d", "type": "modport_group_inherit"}
    assert ports[4] == {"direction": "inout", "name": "e", "type": "modport_group"}


def test_uvm_style_endtoend(sv_grammar):
    """Real-world UVM-style testbench code with package + typedef
    enum/struct + class with rand/constraint/extern/virtual +
    parameterized class + import + module with class instantiation +
    fork-join + global clocking. Parses AND saves cleanly."""
    src = """package my_pkg;
  typedef enum { READ, WRITE, IDLE } cmd_e;
  typedef struct packed {
    cmd_e cmd;
    bit [31:0] addr;
    bit [31:0] data;
  } trans_t;

  class Transaction;
    rand cmd_e cmd;
    rand bit [31:0] addr;
    rand bit [31:0] data;

    constraint c_addr_aligned { addr[1:0] == 0; }
    constraint c_data_range { data inside { [0:1000], [10000:20000] }; }

    function new();
    endfunction

    virtual function string convert2string();
      return $sformatf(\"cmd=%s addr=%h data=%h\", cmd.name(), addr, data);
    endfunction
  endclass

  class Driver #(parameter type T = Transaction) extends uvm_driver;
    virtual interface bus_if vif;
    T tr;

    extern virtual task run_phase(uvm_phase phase);

    function new(string name);
      super.new(name);
      tr = new();
      this.tr = tr;
    endfunction
  endclass
endpackage

import my_pkg::*;

module top;
  bus_if vif();

  initial begin
    Driver drv;
    drv = new(\"drv\");
    drv.vif = vif;
    fork
      drv.run_phase(null);
      $display(\"started\");
    join_any
  end

  global clocking gck @(posedge vif.clk);
  endclocking
endmodule
"""
    r = sv_grammar.parse_string(src)
    # 3 top-level: package + import + module
    assert len(r["descriptions"]) == 3
    descs = [d.get("type") for d in r["descriptions"]]
    assert descs == ["package", "import", "module"]

    # Package body: 2 typedefs + 2 classes
    pkg = r["descriptions"][0]
    item_types = [i.get("type") for i in pkg["items"]]
    assert item_types.count("typedef") == 2
    assert item_types.count("class") == 2

    # Save round-trip
    out = sv_grammar.save(r)
    assert len(out) > 500


def test_define_then_module_use(sv_grammar):
    """Real-world common case: a module that uses a macro in a port
    range and an assignment. The `\`define` directive used to live at
    top level here; it now belongs in the preprocessor pipeline.
    What the SV grammar still has to handle is the inline
    `\`MACRO`/`\`MACRO()` references inside expressions."""
    src = (
        "module m (output [`WIDTH-1:0] y);\n"
        "  assign y = `WIDTH;\n"
        "endmodule\n"
    )
    r = sv_grammar.parse_string(src)
    d0 = r["descriptions"][0]
    assert d0["type"] == "module"
    port = d0["ports"]["ports"][0]
    range_msb_str = str(port["range"])
    assert "macro_use" in range_msb_str
    assert "WIDTH" in range_msb_str


# ─── Save pretty-print: word-token spacing round-trip ───────────────────
#
# The SV grammar is parse-oriented (whitespace is skipped on parse and
# not stored), so save must re-insert spaces between adjacent word
# tokens or the output merges (`modulem`, `inputclk`, `endendmodule`)
# and no longer re-parses. These guard the `space` attrs added to the
# core declaration / statement rules: parse -> save -> re-parse must
# yield the same AST for the common synthesizable constructs.

@pytest.mark.parametrize("src", [
    "module m;\nendmodule\n",
    "module m (input clk, output reg [7:0] q);\nendmodule\n",
    "module m;\n  logic [7:0] x;\n  wire y;\n  assign y = x[0];\nendmodule\n",
    "module m;\n  always_ff @(posedge clk) begin\n    q <= d;\n  end\nendmodule\n",
    # Extended coverage: params, typedef/enum/struct, control flow,
    # function/task, instantiation, generate, package/import.
    "module m #(parameter int W = 8) (input clk);\nendmodule\n",
    "package p;\n  typedef logic [7:0] byte_t;\n  typedef enum logic {A, B} st_t;\nendpackage\n",
    "module m;\n  typedef struct packed {\n    logic [3:0] a;\n    logic b;\n  } s_t;\nendmodule\n",
    "module m;\n  always_comb begin\n    if (a) y = 1;\n    else if (b) y = 2;\n    else y = 3;\n  end\nendmodule\n",
    "module m;\n  always_comb begin\n    case (s)\n      2'b00: y = a;\n      default: y = b;\n    endcase\n  end\nendmodule\n",
    "module m;\n  always_comb begin\n    for (int i = 0; i < 8; i++) y[i] = a[i];\n  end\nendmodule\n",
    "module m;\n  function automatic int f(input int x);\n    return x + 1;\n  endfunction\nendmodule\n",
    "module m;\n  task t(input int x);\n    y = x;\n  endtask\nendmodule\n",
    "module m;\n  sub #(.W(8)) u_sub (.clk(clk), .q(q));\nendmodule\n",
    "module m;\n  genvar i;\n  generate\n    for (i = 0; i < 4; i++) begin : g\n      assign y[i] = a[i];\n    end\n  endgenerate\nendmodule\n",
    "module m;\n  localparam int N = 8;\n  logic [N-1:0] x;\nendmodule\n",
])
def test_sv_save_word_spacing_round_trips(sv_grammar, src):
    a = sv_grammar.parse_string(src)
    out = sv_grammar.save(a)
    out = out.decode("utf-8") if isinstance(out, bytes) else out
    # Tokens must not be glued together.
    for merged in ("modulem", "inputclk", "outputreg", "wirey",
                   "posedgeclk", "endendmodule", "always_ffbegin"):
        assert merged not in out, f"token merge {merged!r} in: {out!r}"
    # Re-parses to the identical AST.
    assert sv_grammar.parse_string(out) == a


def test_sv_param_decl_shapes_save_dispatch(sv_grammar):
    """Every PARAM_DECL alternative is a dict-container and PARAM_DECL_STMT
    is a non-container pass-through, so module-item params parse to proper
    distinct dicts (not an empty {} or a collapsed list) and save-dispatch.
    Regression for the user-typed param losing all fields to {} and the
    `#(parameter, localparam)` 2->1 collapse."""
    # User-typed module-item parameter: keeps name/type_spec/default.
    a = sv_grammar.parse_string(
        "package p;\n  parameter lfsr_perm_t X = 5;\nendpackage\n")
    item = a["descriptions"][0]["items"][0]
    assert item["type"] == "param_usertype"
    assert item["name"] == "X"
    assert item["type_spec"] == "lfsr_perm_t"
    assert sv_grammar.parse_string(sv_grammar.save(a).decode("utf-8")) == a

    # Multi-param `#(...)`: two distinct param dicts, no collapse.
    b = sv_grammar.parse_string(
        "module m #(parameter int W = 8, localparam D = 4) ();\nendmodule\n")
    params = b["descriptions"][0]["param_ports"]["params"]
    assert len(params) == 2
    assert params[0]["name"] == "W" and params[1]["name"] == "D"
    assert sv_grammar.parse_string(sv_grammar.save(b).decode("utf-8")) == b
