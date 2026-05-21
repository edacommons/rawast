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
