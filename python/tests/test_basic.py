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


def test_grammar_default_browses_json_grammar_file_as_dict():
    g = rawast.Grammar()
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


def test_grammar_rawast_browses_rawast_file_as_dict():
    g = rawast.Grammar("rawast")
    data = g.parse_file(rawast.grammar_path("gdsii.rawast"))
    # The .rawast meta-grammar produces the same dict shape as the
    # JSON-form would for an equivalent grammar.
    assert data["LIBRARY"]["type"] == "sequence"
    assert data["LIBRARY"]["container"] == "dict"
    assert len(data["LIBRARY"]["value"]) > 5


def test_from_dict_compiles_runtime_grammar():
    """Inverse of meta.parse_file: turn a dict back into an executable Grammar."""
    meta = rawast.Grammar("rawast")
    data = meta.parse_file(rawast.grammar_path("gdsii.rawast"))
    g = rawast.Grammar.from_dict(data)
    # The reconstituted grammar should lint cleanly (same as the
    # original gdsii.rawast loaded directly).
    assert g.lint() == []


def test_to_dict_round_trips_loaded_grammar():
    """Walk a loaded Grammar back to dict form, rebuild from it, verify
    the round-tripped Grammar lints clean and parses the same input.

    The load-bearing invariant for `rawast cppgen` (issue #2) and the
    `.jast` writer (M2): `to_dict()` is the inverse of `from_dict()`.
    """
    g1 = rawast.Grammar("json")
    d = g1.to_dict()
    assert isinstance(d, dict)
    assert "start" in d
    assert d["start"]   # non-empty start rule name

    g2 = rawast.Grammar.from_dict(d)
    assert g2.lint() == []
    assert g1.parse_string('{"a": 1}') == g2.parse_string('{"a": 1}')


def test_to_dict_preserves_repeat_plus_n():
    """The repeat+N quantifier (0.1.2) round-trips through to_dict."""
    meta = rawast.Grammar("rawast")
    src = (
        "use: std\n"
        "start: <X>\n"
        "X: sequence { repeat+3 int:nums[]=@ separator \",\" }\n"
    )
    import tempfile, os
    with tempfile.NamedTemporaryFile("w", suffix=".rawast", delete=False) as f:
        f.write(src)
        path = f.name
    try:
        dict_form = meta.parse_file(path)
        g = rawast.Grammar.from_dict(dict_form)
        round_tripped = g.to_dict()
        assert round_tripped["X"]["value"][0]["min"] == 3
    finally:
        os.unlink(path)


def test_from_dict_transformation():
    """The 'grammars are data' story: load → transform → rebuild."""
    meta = rawast.Grammar("rawast")
    data = meta.parse_file(rawast.grammar_path("gdsii.rawast"))

    # Add a synthetic rule.
    data["EXTRA"] = {"type": "key", "key": "extra"}

    g = rawast.Grammar.from_dict(data)
    assert g.lint() == []   # still well-formed after transformation


def test_meta_grammar_design_matches_runtime():
    """rawast.rawast (design) and rawast.json (runtime) must encode the
    same dict tree. Parsing rawast.rawast via the runtime meta-grammar
    must yield exactly what rawast.json says — every rule, byte-equal."""
    meta = rawast.Grammar("rawast")
    parsed = meta.parse_file(rawast.grammar_path("rawast.rawast"))
    with open(rawast.grammar_path("rawast.json")) as f:
        canonical = json.load(f)
    assert parsed == canonical


def test_schema_generator_emits_markdown_for_every_bundled_grammar():
    """`rawast.schema.to_markdown` walks any loaded grammar dict and
    emits a value-tree-shape Markdown reference. Smoke-test against
    every bundled grammar (json, rawast, gdsii, lef, def, tcl)."""
    from rawast.schema import to_markdown
    meta = rawast.Grammar("rawast")
    json_meta = rawast.Grammar()
    cases = [
        ("json.json",   json_meta),
        ("rawast.rawast", meta),
        ("gdsii.rawast", meta),
        ("lefdef.rawast",   meta),
        ("tcl.rawast",   meta),
    ]
    for name, loader in cases:
        data = loader.parse_file(rawast.grammar_path(name))
        md = to_markdown(data, title=name)
        assert "value-tree shape" in md
        assert "**Start rule:**" in md
        # Every grammar declares at least one rule, so the per-rule
        # heading marker must appear.
        assert "\n## " in md, f"no per-rule headings for {name}"


def test_schema_generator_describes_json_paircase():
    """The JSON PAIR rule binds a string as a dict key via the
    `{"name": "", "value": "@"}` var-marker convention; the schema
    generator should render that as a dynamic-key dict entry whose
    value is `VALUE`."""
    from rawast.schema import to_markdown
    g = rawast.Grammar()
    data = g.parse_file(rawast.grammar_path("json.json"))
    md = to_markdown(data, title="JSON")
    # The PAIR section should reference VALUE as the value side of
    # the dynamic-key entry, not the `":"` structural key.
    pair_idx = md.find("## PAIR")
    next_idx = md.find("\n## ", pair_idx + 1)
    pair_section = md[pair_idx:next_idx]
    assert "VALUE" in pair_section
    assert '":"' not in pair_section  # `:` is structural, skipped
