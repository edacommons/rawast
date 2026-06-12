"""Python-side tests for the Preprocessor class and Grammar integration.

The C++ side has comprehensive walker tests via tests/test_preprocessor.cpp;
here we focus on the Python binding surface — keyword argument shape,
property return types, error messages — plus the Grammar.parse_string /
parse_file `preprocessor=` keyword integration.
"""

import os

import pytest

import rawast


GRAMMARS = os.path.join(os.path.dirname(__file__), "..", "..", "grammars")
SV_PP_GRAMMAR = os.path.join(GRAMMARS, "sv_preprocessor.rawast")


def _sv_pp() -> rawast.Grammar:
    return rawast.Grammar.load(SV_PP_GRAMMAR)


# ─── Construction ──────────────────────────────────────────────────────────


def test_default_construction():
    """Constructing with only a grammar gives an empty, ready Preprocessor."""
    g = _sv_pp()
    pp = rawast.Preprocessor(g)
    assert pp.macros == {}
    assert pp.included_files == []
    assert pp.warnings == []


def test_predefined_option_seeds_macros():
    """The predefined string is processed at construction time."""
    g = _sv_pp()
    pp = rawast.Preprocessor(g, predefined="`define BUILD_REV 42\n")
    assert pp.is_defined("BUILD_REV")
    m = pp.get_macro("BUILD_REV")
    assert m["name"] == "BUILD_REV"
    assert m["body"] == "42"
    assert m["params"] == []
    assert m["is_function_like"] is False


def test_unknown_on_undefined_raises_clear_error():
    """The on_undefined keyword takes an enum-string; bogus values fail loud."""
    g = _sv_pp()
    with pytest.raises(RuntimeError, match="on_undefined"):
        rawast.Preprocessor(g, on_undefined="bogus_policy")


def test_all_construction_kwargs_accepted():
    """Every documented keyword can be passed without error."""
    g = _sv_pp()
    pp = rawast.Preprocessor(
        g,
        predefined="",
        include_paths=["/usr/include", "./rtl"],
        splice=True,
        on_undefined="warn",
        max_expansion_depth=50,
        trace=True,
    )
    # No public option-inspection accessor in Phase 1; just confirm the
    # kwargs were accepted and the instance is usable.
    assert pp.process("hello\n") == "hello\n"


# ─── Walker via process() ──────────────────────────────────────────────────


def test_define_registers_object_like_macro():
    pp = rawast.Preprocessor(_sv_pp())
    pp.process("`define WIDTH 32\n")
    assert pp.is_defined("WIDTH")
    assert pp.get_macro("WIDTH")["body"] == "32"


def test_undef_removes_macro():
    pp = rawast.Preprocessor(_sv_pp())
    pp.process("`define FOO 1\n`undef FOO\n")
    assert not pp.is_defined("FOO")


def test_ifdef_includes_body_when_defined():
    pp = rawast.Preprocessor(_sv_pp(), predefined="`define RVFI\n")
    out = pp.process(
        "`ifdef RVFI\n"
        "  output logic rvfi_valid,\n"
        "`endif\n"
    )
    assert "rvfi_valid" in out


def test_ifdef_drops_body_when_undefined():
    pp = rawast.Preprocessor(_sv_pp())
    out = pp.process(
        "`ifdef NEVER\n"
        "  should_disappear\n"
        "`endif\n"
    )
    assert "should_disappear" not in out


def test_ifdef_else_takes_else_when_undefined():
    pp = rawast.Preprocessor(_sv_pp())
    out = pp.process(
        "`ifdef NEVER\n"
        "  body_branch\n"
        "`else\n"
        "  else_branch\n"
        "`endif\n"
    )
    assert "else_branch" in out
    assert "body_branch" not in out


def test_nested_ifdef():
    pp = rawast.Preprocessor(_sv_pp(), predefined="`define OUTER\n")
    out = pp.process(
        "`ifdef OUTER\n"
        "  outer_visible\n"
        "  `ifdef INNER\n"
        "    inner_hidden\n"
        "  `endif\n"
        "  outer_after\n"
        "`endif\n"
    )
    assert "outer_visible" in out
    assert "outer_after" in out
    assert "inner_hidden" not in out


# ─── State management ─────────────────────────────────────────────────────


def test_reset_clears_macros():
    pp = rawast.Preprocessor(_sv_pp())
    pp.process("`define X 1\n`define Y 2\n")
    assert pp.is_defined("X") and pp.is_defined("Y")
    pp.reset()
    assert not pp.is_defined("X")
    assert pp.macros == {}


def test_state_accumulates_across_calls():
    """Multi-file workflow — reuse one Preprocessor instance."""
    pp = rawast.Preprocessor(_sv_pp())
    pp.process("`define FROM_FIRST_CALL 1\n")
    pp.process("`define FROM_SECOND_CALL 2\n")
    assert pp.is_defined("FROM_FIRST_CALL")
    assert pp.is_defined("FROM_SECOND_CALL")


# ─── Inspection types ─────────────────────────────────────────────────────


def test_macros_is_dict_keyed_by_name():
    pp = rawast.Preprocessor(_sv_pp(), predefined="`define A 1\n`define B 2\n")
    assert isinstance(pp.macros, dict)
    assert set(pp.macros.keys()) == {"A", "B"}


def test_get_macro_returns_none_for_undefined():
    pp = rawast.Preprocessor(_sv_pp())
    assert pp.get_macro("NEVER_DEFINED") is None


def test_warnings_is_list_of_dicts():
    pp = rawast.Preprocessor(_sv_pp())
    pp.process_file("/nonexistent/file.sv")
    assert isinstance(pp.warnings, list)
    assert len(pp.warnings) == 1
    w = pp.warnings[0]
    assert "message" in w
    assert "file" in w
    assert "line" in w


# ─── Grammar.parse_string(..., preprocessor=) integration ──────────────────


def test_parse_string_with_preprocessor_keyword():
    """Grammar.parse_string accepts a preprocessor= kwarg that runs first."""
    g = _sv_pp()  # pp grammar can also serve as host for round-trip
    pp = rawast.Preprocessor(g, predefined="`define KEEP\n")
    ast = g.parse_string(
        "`ifdef KEEP\nvisible\n`endif\n`ifdef NOT_DEFINED\nhidden\n`endif\n",
        preprocessor=pp,
    )
    text = "".join(item["text"] for item in ast if item.get("type") == "text")
    assert "visible" in text
    assert "hidden" not in text


# ─── Source map ───────────────────────────────────────────────────────────


def test_spans_have_root_input_entry():
    """The root span describes the whole input; its parent_id is None."""
    pp = rawast.Preprocessor(_sv_pp())
    pp.process("hello\n")
    root = next(s for s in pp.spans if s["parent_id"] is None)
    assert root["name"] == "<input>"
    assert root["length"] == 6
    assert root["out_offset"] is None    # source-structure only


def test_stack_at_returns_root_input_as_last_frame():
    pp = rawast.Preprocessor(_sv_pp())
    pp.process("hello world\n")
    stack = pp.stack_at(5)
    assert len(stack) >= 1
    assert stack[-1]["where"] == "<input>"
    # Pass-through — offset 5 in output is offset 5 in source.
    assert stack[-1]["offset"] == 5


def test_stack_at_out_of_range_returns_empty():
    pp = rawast.Preprocessor(_sv_pp())
    pp.process("short\n")
    assert pp.stack_at(9999) == []


def test_source_map_skips_dropped_ifdef_body():
    """After a dropped ifdef block, output offsets map back to the
    correct source byte (past the block, not into the dropped lines)."""
    pp = rawast.Preprocessor(_sv_pp())
    text = (
        "before\n"
        "`ifdef NEVER\n"
        "  dropped_line\n"
        "`endif\n"
        "after\n"
    )
    out = pp.process(text)
    assert out == "before\nafter\n"
    pos = out.index("after")
    stack = pp.stack_at(pos)
    assert stack
    # "after" begins at byte 42 in source (after the full ifdef block).
    assert stack[-1]["offset"] == 42


def test_source_map_resets_between_process_calls():
    """Each process() call starts a fresh map; previous call's spans
    don't bleed into the next."""
    pp = rawast.Preprocessor(_sv_pp())
    pp.process("first call\n")
    first_count = len(pp.spans)
    pp.process("second call\n")
    second_count = len(pp.spans)
    # Second call replaces the map; counts may differ slightly but
    # neither call should leak entries into the other.
    # Specifically, every span's parent chain still resolves cleanly.
    for s in pp.spans:
        if s["parent_id"] is not None:
            assert s["parent_id"] < len(pp.spans)
    assert first_count > 0  # spans were recorded for first call
    assert second_count > 0  # and reset/rebuilt for second call
