"""SV preprocessor expansion regressions."""
from __future__ import annotations

import rawast


def _expand(src: str) -> str:
    pp = rawast.Grammar("sv_preprocessor")
    p = rawast.Preprocessor(pp, predefined="", include_paths=[],
                            on_undefined="leave")
    out = p.process(src)
    return out.decode("utf-8") if isinstance(out, (bytes, bytearray)) else out


def test_line_continuation_in_nested_macro_call_args():
    r"""`\<newline>` line-continuations inside a NESTED macro call's argument
    list (within a macro body) must be joined, like top-level body
    continuations. Regression: UVM `uvm_field_int` expands
    `uvm_record_int("ARG", \<nl> ARG, \<nl> $bits(ARG), ...)` and the stray
    `\` used to leak into the expansion, breaking the SV parse."""
    src = (
        "`define C(a, b) c(a, b);\n"
        "`define F `C(p, \\\n           q)\n"
        "initial `F\n"
    )
    out = _expand(src)
    assert "\\" not in out, f"stray backslash in: {out!r}"
    assert "c(p, q);" in out or "c(p,  q);" in out


def test_line_continuation_top_level_and_outside_calls_still_join():
    """Sanity: the existing (already-working) continuation cases still join."""
    assert "\\" not in _expand("`define M a \\\n b \\\n c\nx = `M;\n")
    out = _expand("`define C(a,b) c(a,b);\n`define F `C(p,q) \\\n extra\ninitial `F\n")
    assert "\\" not in out


def test_stringify_in_nested_macro_call_args():
    """A `\\`"ARG`"` stringify passed as an argument to a NESTED macro call
    must have the outer param substituted before the inner call expands.
    Regression: UVM `uvm_field_int` expands `\\`uvm_record_int(\\`"ARG`",
    ARG, ...)`; the stringify operand used to keep the literal param name
    ("ARG") through deep nesting, leaking `\\`"ARG`"` into the output."""
    bt = chr(96)
    src = ("`define GEN(n) g(n);\n"
           "`define REC(name) `GEN(name)\n"
           "`define FIELD(ARG) `REC(" + bt + '"ARG' + bt + '")\n'
           "initial `FIELD(myf)\n")
    out = _expand(src)
    assert 'g("myf");' in out, out
    assert bt not in out and "ARG" not in out


def test_function_like_call_with_space_before_paren():
    """LRM §22.5.1 permits whitespace between a function-like macro name and
    its `(args)` at the use site (`\\`uvm_field_int (saw_error)`). At file
    scope the grammar parses `\\`name` as object-like + ` (args)` text; the
    engine lifts the args into the call when the macro is function-like.
    Object-like uses keep verbatim spacing."""
    fl = ("`define M(x, y) f(x, y);\n"
          "initial `M (a, b)\n")
    out = _expand(fl)
    assert "f(a, b);" in out, out
    # object-like macro followed by parenthesised text — spacing preserved
    ol = "`define A alpha\n`define B beta\nx = `A + `B;\n"
    assert "x = alpha + beta;" in _expand(ol)


def test_inline_ifdef_in_expression():
    r"""A conditional directive embedded MID-LINE in an expression value —
    `localparam int X = \`ifdef Y 2 \`else 1 \`endif;` (PULP/Snitch). The
    line-oriented preprocessor used to sweep the whole line into TEXT_LINE
    and leak `\`ifdef` to the SV parser; now it's normalized onto its own
    lines and evaluated. Line-start conditionals are untouched."""
    bt = chr(96)
    undef = "localparam int X = " + bt + "ifdef Y 2 " + bt + "else 1 " + bt + "endif;\n"
    out = _expand(undef)
    assert bt not in out and "1" in out and "2" not in out  # Y undefined -> else

    defd = bt + "define Y\nx = " + bt + "ifdef Y 2 " + bt + "else 1 " + bt + "endif;\n"
    out = _expand(defd)
    assert bt not in out and "2" in out  # Y defined -> then
