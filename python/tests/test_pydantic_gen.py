"""Round-trip tests for `rawast.pydantic_gen`.

The contract the generator must satisfy:

    Class.model_validate(g.parse_file(path)).model_dump(...) == g.parse_file(path)

i.e. parsing → validating through the generated model → dumping back
yields the *same* dict the engine produced. The Pydantic model IS
exactly the shape the grammar can save; the user can only construct
what the grammar can serialise.

These tests use a small inline LEF that exercises the catcher pattern
(SITE_BLOCK with multiple SITE_PROPERTYs flowing into the parent dict).
"""

from __future__ import annotations

import importlib.util
import pathlib

import pytest

import rawast


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
GRAMMARS = REPO_ROOT / "grammars"


MIN_LEF = """\
VERSION 5.8 ;
BUSBITCHARS "[]" ;
DIVIDERCHAR "/" ;

UNITS
  DATABASE MICRONS 1000 ;
END UNITS

SITE core
  CLASS CORE ;
  SYMMETRY Y ;
  SIZE 0.46 BY 2.72 ;
END core

END LIBRARY
"""


def _generate_lef_models(tmp_path):
    """Generate `lef_models.py` under tmp_path and import it as a module."""
    from rawast.pydantic_gen import to_pydantic
    meta = rawast.Grammar("rawast")
    grammar = meta.parse_file(str(GRAMMARS / "lef.rawast"))
    src = to_pydantic(grammar)
    module_path = tmp_path / "lef_models.py"
    module_path.write_text(src)
    spec = importlib.util.spec_from_file_location("lef_models", module_path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_round_trip_min_lef(tmp_path):
    """The core contract: parse → validate → dump == parsed."""
    pytest.importorskip("pydantic")
    lef_path = tmp_path / "min.lef"
    lef_path.write_text(MIN_LEF)
    g = rawast.Grammar.load(str(GRAMMARS / "lef.rawast"))
    parsed = g.parse_file(str(lef_path))

    mod = _generate_lef_models(tmp_path)
    model = mod.Library.model_validate(parsed)
    dumped = model.model_dump(exclude_none=True, by_alias=True)

    assert dumped == parsed, (
        f"Round-trip mismatch.\n"
        f"  parsed: {parsed}\n"
        f"  dumped: {dumped}\n"
        f"  diff in dumped only: "
        f"{set(dumped) ^ set(parsed) if isinstance(dumped, dict) else '(structure differs)'}"
    )


def test_user_can_construct_site_block(tmp_path):
    """User-facing construction story for SITE_BLOCK."""
    pytest.importorskip("pydantic")
    mod = _generate_lef_models(tmp_path)
    site = mod.SiteBlock(
        name="core",
        end_name="core",
        symmetry=["Y"],
        width=0.46,
        height=2.72,
        **{"class": "CORE"},  # `class` is a Python keyword, pass via dict
    )
    dumped = site.model_dump(exclude_none=True, by_alias=True)
    assert dumped == {
        "type": "Site",
        "name": "core",
        "class": "CORE",
        "symmetry": ["Y"],
        "width": 0.46,
        "height": 2.72,
        "end_name": "core",
    }


def test_extra_field_is_rejected(tmp_path):
    """Contract: model cannot accept fields the grammar can't save."""
    pytest.importorskip("pydantic")
    mod = _generate_lef_models(tmp_path)
    from pydantic import ValidationError
    with pytest.raises(ValidationError):
        mod.SiteBlock(name="core", end_name="core", garbage_field="x")


def test_container_less_rules_are_not_emitted_as_classes(tmp_path):
    """Per design: rules like SITE_SIZE, SITE_CLASS, VERSION_CMD have
    no standalone class — their fields appear in the parent."""
    pytest.importorskip("pydantic")
    mod = _generate_lef_models(tmp_path)
    # These are container-less sequence rules in lef.rawast.
    assert not hasattr(mod, "SiteSize")
    assert not hasattr(mod, "SiteClass")
    assert not hasattr(mod, "VersionCmd")
    # The catcher-only choice rule SITE_PROPERTY also has no class.
    assert not hasattr(mod, "SiteProperty")
