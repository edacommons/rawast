"""Phase B save engine — acceptance tests.

The save entry point is the stack-navigation rewrite of the save
direction. These tests pin its capabilities:

  * Self-host: the .rawast meta-grammar parses its own grammar files
    and saves them back as .rawast text that re-parses to the same
    dict (round-trip).
  * Existing JSON round-trip via save (no regression).
  * GDSII round-trip via save (no regression).
"""

from __future__ import annotations

import json
import pathlib

import pytest

import rawast


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
GRAMMARS  = REPO_ROOT / "grammars"


# --- The headline Phase B test: self-host save ----------------------------

def test_self_host_save_gdsii_rawast():
    """Parse gdsii.rawast through the meta-grammar, save as .rawast text
    via save, re-parse — the dicts must be equal."""
    meta = rawast.Grammar("rawast")
    original = meta.parse_file(str(GRAMMARS / "gdsii.rawast"))
    text     = meta.save(original, pretty=True).decode("utf-8")
    reparsed = meta.parse_string(text)
    assert reparsed == original


def test_self_host_save_includes_use_directive():
    """The `use: gdsii` directive at the top of gdsii.rawast must
    survive the round-trip."""
    meta = rawast.Grammar("rawast")
    original = meta.parse_file(str(GRAMMARS / "gdsii.rawast"))
    text     = meta.save(original, pretty=True).decode("utf-8")
    assert "use" in text
    assert "gdsii" in text


# --- JSON via save (no regression vs the legacy engine) ----------------

@pytest.mark.parametrize("value", [
    42,
    "hello",
    True,
    False,
    [1, 2, 3],
    {"a": 1},
    {"a": 1, "b": [2, 3], "c": "nested"},
    {"nested": {"deeper": {"deepest": [1, 2, [3, 4]]}}},
])
def test_json_save_round_trip(value):
    g = rawast.Grammar.load(str(GRAMMARS / "json.json"))
    raw = g.save(value).decode("utf-8")
    assert json.loads(raw) == value


# --- save dispatch behaviours -----------------------------------------

def test_save_dispatches_parse_expr_catch_all():
    """For `{"type":"int"}`, the EXPR Choice picks PARSE_EXPR
    (catch-all alternative)."""
    meta = rawast.Grammar("rawast")
    data = {"start": {"type": "X"}, "X": {"type": "int"}}
    text = meta.save(data, pretty=False).decode("utf-8")
    assert "int" in text


def test_save_dispatches_ref_for_ref_dict():
    """In EXPR's choice, a flat ref dict {"type": NAME} picks REF
    (matches the `<identifier>` shape)."""
    meta = rawast.Grammar("rawast")
    data = {
        "start": {"type": "X"},
        "X": {"type": "Y"},
        "Y": {"type": "identifier"},
    }
    text = meta.save(data, pretty=False).decode("utf-8")
    assert "<Y>" in text or "<X>" in text


def test_save_dispatches_use_decl_via_bare_key():
    """`USE_DECL` matches via key-based dispatch — the bare Key 'use'
    in its grammar tree matches when the current dict key is 'use'."""
    meta = rawast.Grammar("rawast")
    data = {
        "start": {"type": "X"},
        "use": ["gdsii"],
        "X": {"type": "identifier"},
    }
    text = meta.save(data, pretty=False).decode("utf-8")
    assert "use" in text and "gdsii" in text


def test_save_bare_item_emits_no_binding_suffix():
    """A flat-form ITEM with no `bindings` field should emit just the
    expr — no `:name=` suffix."""
    meta = rawast.Grammar("rawast")
    data = {
        "start": {"type": "X"},
        "X": {"type": "sequence", "container": "array", "value": [
            {"type": "repeat", "value": {"type": "Y"}}
        ]},
        "Y": {"type": "identifier"},
    }
    text = meta.save(data, pretty=False).decode("utf-8")
    # Bare items emit just the expr — no `:name=` binding suffix.
    # The bare repeat item `<Y>` must render without a trailing
    # `:...` binding. (Rule-def colons render `X : …` / `Y : …` with
    # a space since the meta-grammar now spaces after the rule name
    # so `NAME ignore …:` round-trips; that's unrelated to bindings.)
    assert "<Y>" in text
    assert "<Y>:" not in text       # no binding suffix on the bare item
    assert "=" not in text          # no binding has a value


# --- opchain: implication `->`/`<->` round-trips (regression) -------------

def test_opchain_implication_round_trips():
    """`->`/`<->` is the loosest EXPR cascade tier, ABOVE the #opchain
    mark on BIN_EXPR. compact_opchain used to fold it into `{op,args}`
    anyway, but the save-side cascade ladder has no tier for it, so saving
    any `a -> b` died with "no matching grammar alternative ... POWER_EXPR".
    The fold is now restricted to ladder ops, leaving implication
    always-wrap so it round-trips via IMPL_CHAIN."""
    sv = rawast.Grammar("systemverilog")
    W = "class c; function void f(); %s endfunction endclass"
    for stmt in [
        "y = a -> b;",
        "y = (!g) -> d == 0;",      # implication with a comparison consequent
        "y = a <-> b;",             # equivalence
        "y = a -> b -> c;",         # right-chained implication
        "y = a || b && c;",         # ordinary binary cascade still folds
    ]:
        ast = sv.parse_string(W % stmt)
        assert sv.parse_string(sv.save(ast).decode("utf-8")) == ast
