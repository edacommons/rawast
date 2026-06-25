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
