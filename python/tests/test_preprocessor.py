"""SV preprocessor expansion regressions."""
from __future__ import annotations

import rawast


def _expand(src: str) -> str:
    pp = rawast.Grammar("systemverilog")
    p = rawast.Preprocessor(pp, predefined="", include_paths=[],
                            on_undefined="leave")
    out = p.process(src)
    return out.decode("utf-8") if isinstance(out, (bytes, bytearray)) else out


def test_formals_not_substituted_inside_plain_string_in_nested_call():
    r"""Macro formals must NOT be substituted inside a PLAIN string literal
    (a string is one lexical token; only `\`"…\`"` stringify substitutes).
    The direct case already works via segmentation, but a nested macro
    call captures its arg as raw text, and substitute_params crossed the
    string boundary. Regression: UVM `uvm_field_int(ARG,FLAG)` wraps a
    warning `"Field macro for ARG uses FLAG ..."` — a plain string whose
    words happen to match the formals; we rewrote it to the actual args
    (Verilator, correctly, does not). Found by the Verilator oracle."""
    src = (
        "`define INNER(msg) $warning(msg)\n"
        "`define OUTER(ARG, FLAG) `INNER(\"text ARG uses FLAG here\")\n"
        "`OUTER(x, y)\n"
    )
    out = _expand(src)
    assert '"text ARG uses FLAG here"' in out, out
    assert "text x uses y" not in out, out

    # `\`"…\`"` stringify STILL substitutes (must not regress):
    strf = (
        "`define S(A) `\"got A`\"\n"
        "`S(z)\n"
    )
    assert '"got z"' in _expand(strf)


def test_get_macro_on_function_like_returns_params():
    """get_macro() / .macros must expose a function-like macro's parameters
    as data, not throw. Regression: the binding appended raw MacroParam
    structs (no nanobind caster) so any macro WITH params raised
    std::bad_cast; object-like macros happened to work only because the
    params loop never ran."""
    pp = rawast.Grammar("systemverilog")
    p = rawast.Preprocessor(pp, on_undefined="leave")
    p.process("`define FOO 1\n`define ADD(a, b) (a + b)\n")

    foo = p.get_macro("FOO")
    assert foo is not None
    assert foo["is_function_like"] is False
    assert foo["params"] == []

    add = p.get_macro("ADD")
    assert add is not None
    assert add["is_function_like"] is True
    assert [param["name"] for param in add["params"]] == ["a", "b"]

    # the .macros property walks the same conversion — must not throw either
    assert set(p.macros) == {"FOO", "ADD"}
    assert [x["name"] for x in p.macros["ADD"]["params"]] == ["a", "b"]


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


def test_token_paste_constructs_and_reexpands_macro_name():
    r"""The `` token-paste operator (LRM §22.5.1) can build a macro NAME
    from fragments — lowRISC's `DV_CHECK` does `` `dv_``SEV_(...) `` so
    `SEV_=fatal` yields `` `dv_fatal(...) ``. The constructed name must be
    resolved and expanded, not left as `` `dv_ `` next to the text `fatal`.
    Fixed by folding `macro_use `` frag` into a constructed macro use in the
    segment domain (no text re-parse)."""
    src = ("`define dv_error(M) real_error(M)\n"
           "`define dv_fatal(M) real_fatal(M)\n"
           "`define CHK(SEV) `dv_``SEV(hi)\n"
           "initial `CHK(error);\n"
           "initial `CHK(fatal);\n")
    out = _expand(src)
    assert "real_error(hi)" in out and "real_fatal(hi)" in out, out
    assert "`dv_" not in out, out


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
