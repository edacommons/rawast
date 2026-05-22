"""Basic smoke tests for the Python binding.

Run with: pytest python/tests/
"""

from __future__ import annotations

import json
import os
import pathlib

import pytest

import rawast


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
GRAMMARS = REPO_ROOT / "grammars"


def test_version_string():
    assert isinstance(rawast.__version__, str)
    assert rawast.__version__


def test_load_json_grammar():
    g = rawast.Grammar.load(str(GRAMMARS / "json.json"))
    assert g is not None


def test_parse_json_string():
    g = rawast.Grammar.load(str(GRAMMARS / "json.json"))
    ast = g.parse_string('{"name": "alice", "items": [1, 2, 3], "ok": true}')
    assert ast == {"name": "alice", "items": [1, 2, 3], "ok": True}


def test_parse_json_primitives():
    g = rawast.Grammar.load(str(GRAMMARS / "json.json"))
    assert g.parse_string("42") == 42
    assert g.parse_string("3.14") == pytest.approx(3.14)
    assert g.parse_string('"hello"') == "hello"
    assert g.parse_string("null") is None
    assert g.parse_string("true") is True
    assert g.parse_string("false") is False


def test_save_json_roundtrip():
    g = rawast.Grammar.load(str(GRAMMARS / "json.json"))
    value = {"a": 1, "b": [2, 3], "c": None, "d": "x"}
    raw = g.save(value)
    assert isinstance(raw, bytes)
    text = raw.decode("utf-8")
    # Should be valid JSON expressing the same value.
    assert json.loads(text) == value


def test_lint_clean_grammar():
    g = rawast.Grammar.load(str(GRAMMARS / "json.json"))
    assert g.lint() == []


def test_lint_gdsii_grammar():
    # Exercises `use: gdsii` resolution + the lint pass.
    g = rawast.Grammar.load(str(GRAMMARS / "gdsii.rawast"))
    assert g.lint() == []


def test_load_rawast_meta_grammar():
    # Self-host check: load the .rawast meta-grammar.
    g = rawast.Grammar.load(str(GRAMMARS / "rawast.json"))
    assert g is not None


def test_parse_failure_raises():
    g = rawast.Grammar.load(str(GRAMMARS / "json.json"))
    with pytest.raises(RuntimeError, match="byte"):
        g.parse_string("not valid json")


# --- Grammar introspection: meta-grammars + Grammar.from_dict --------------

def test_grammar_path_resolves_bundled_files():
    """Grammar files ship with the package via the symlink/install rule."""
    path = rawast.grammar_path("json.json")
    assert os.path.exists(path), f"{path} should be bundled with rawast"


def test_json_format_browses_json_grammar_file_as_dict():
    g = rawast.json_format()
    data = g.parse_file(rawast.grammar_path("json.json"))
    # The grammar definition itself comes through as a Python dict —
    # we can walk it like any other parsed value. Items use the
    # multi-binding wrapper form ({"expr": <X>, "bindings": {...}?}).
    # The `bindings` field is omitted on bare items.
    assert "VALUE" in data
    assert data["VALUE"]["type"] == "choice"
    items = data["VALUE"]["items"]
    expr_refs = [it["expr"] for it in items if "bindings" not in it]
    assert "STRUCT" in expr_refs


def test_rawast_format_browses_rawast_file_as_dict():
    g = rawast.rawast_format()
    data = g.parse_file(rawast.grammar_path("gdsii.rawast"))
    # The .rawast meta-grammar produces the same dict shape as the
    # JSON-form would for an equivalent grammar.
    assert data["LIBRARY"]["type"] == "sequence"
    assert data["LIBRARY"]["container"] == "dict"
    assert len(data["LIBRARY"]["value"]) > 5


def test_from_dict_compiles_runtime_grammar():
    """Inverse of meta.parse_file: turn a dict back into an executable Grammar."""
    meta = rawast.rawast_format()
    data = meta.parse_file(rawast.grammar_path("gdsii.rawast"))
    g = rawast.Grammar.from_dict(data)
    # The reconstituted grammar should lint cleanly (same as the
    # original gdsii.rawast loaded directly).
    assert g.lint() == []


def test_from_dict_transformation():
    """The 'grammars are data' story: load → transform → rebuild."""
    meta = rawast.rawast_format()
    data = meta.parse_file(rawast.grammar_path("gdsii.rawast"))

    # Add a synthetic rule.
    data["EXTRA"] = {"type": "key", "key": "extra"}

    g = rawast.Grammar.from_dict(data)
    assert g.lint() == []   # still well-formed after transformation
