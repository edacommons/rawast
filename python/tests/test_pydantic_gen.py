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


MACRO_LEF = """\
VERSION 5.8 ;
NAMESCASESENSITIVE ON ;
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

MACRO INV_X1
  CLASS CORE ;
  ORIGIN 0 0 ;
  FOREIGN INV_X1 0 0 ;
  SIZE 1.84 BY 2.72 ;
  SYMMETRY X Y ;
  SITE core ;
  PIN A
    DIRECTION INPUT ;
    USE SIGNAL ;
    PORT
      LAYER met1 ;
        RECT 0.1 0.4 0.3 0.6 ;
    END
  END A
  PIN Y
    DIRECTION OUTPUT ;
    USE SIGNAL ;
    PORT
      LAYER met1 ;
        RECT 1.4 1.0 1.7 1.3 ;
    END
  END Y
END INV_X1

END LIBRARY
"""


def test_round_trip_macro_lef(tmp_path):
    """Round-trip a fuller LEF with MACRO, PINs, multiple SITE_PROPERTYs,
    SYMMETRY with multiple identifiers, RECT shapes inside PORT/LAYER."""
    pytest.importorskip("pydantic")
    lef_path = tmp_path / "macro.lef"
    lef_path.write_text(MACRO_LEF)
    g = rawast.Grammar.load(str(GRAMMARS / "lef.rawast"))
    parsed = g.parse_file(str(lef_path))

    mod = _generate_lef_models(tmp_path)
    model = mod.Library.model_validate(parsed)
    dumped = model.model_dump(exclude_none=True, by_alias=True)

    assert dumped == parsed


def test_user_can_construct_macro_with_pins(tmp_path):
    """User constructs an INV-style MACRO with two PINs from scratch."""
    pytest.importorskip("pydantic")
    mod = _generate_lef_models(tmp_path)
    macro = mod.MacroBlock(
        name="INV_X1",
        end_name="INV_X1",
        width=1.84,
        height=2.72,
        symmetry=["X", "Y"],
        site="core",
        origin_x=0, origin_y=0,
        foreign_cell="INV_X1", foreign_x=0, foreign_y=0,
        pins=[
            mod.PinBlock(
                name="A", end_name="A",
                direction="INPUT", use="SIGNAL",
                ports=[mod.PortBlock(
                    layer_groups=[mod.LayerGroup(
                        layer="met1",
                        shapes=[mod.RectShape(
                            x1=0.1, y1=0.4, x2=0.3, y2=0.6)],
                    )],
                )],
            ),
        ],
        **{"class": ["CORE"]},  # `class` keyword alias
    )
    dumped = macro.model_dump(exclude_none=True, by_alias=True)
    # Spot-check key fields
    assert dumped["type"] == "Macro"
    assert dumped["name"] == "INV_X1"
    assert dumped["class"] == ["CORE"]
    assert dumped["pins"][0]["name"] == "A"
    assert dumped["pins"][0]["ports"][0]["layer_groups"][0]["shapes"][0]["type"] == "Rect"


OBS_LEF = """\
VERSION 5.8 ;

UNITS
  DATABASE MICRONS 1000 ;
END UNITS

MACRO INV_X1
  CLASS CORE ;
  SIZE 1.84 BY 2.72 ;
  PIN A
    DIRECTION INPUT ;
    USE SIGNAL ;
    PORT
      LAYER met1 ;
        RECT 0.1 0.4 0.3 0.6 ;
    END
  END A
  OBS
    LAYER met1 ;
      RECT 0.0 0.0 1.84 0.05 ;
  END
END INV_X1

END LIBRARY
"""


def test_round_trip_macro_with_obs(tmp_path):
    """OBS section round-trip: `?<MACRO_OBS>:obs=@` in lef.rawast captures
    OBS as a sub-dict; MacroBlock has `obs: MacroObs | None = None`."""
    pytest.importorskip("pydantic")
    lef_path = tmp_path / "obs.lef"
    lef_path.write_text(OBS_LEF)
    g = rawast.Grammar.load(str(GRAMMARS / "lef.rawast"))
    parsed = g.parse_file(str(lef_path))

    mod = _generate_lef_models(tmp_path)
    model = mod.Library.model_validate(parsed)
    dumped = model.model_dump(exclude_none=True, by_alias=True)

    assert dumped == parsed


def test_user_can_construct_macro_with_obs(tmp_path):
    """Users build OBS nested inside MACRO via the MacroObs class."""
    pytest.importorskip("pydantic")
    mod = _generate_lef_models(tmp_path)
    macro = mod.MacroBlock(
        name="INV_X1",
        end_name="INV_X1",
        width=1.84,
        height=2.72,
        pins=[],
        obs=mod.MacroObs(layer_groups=[mod.LayerGroup(
            layer="met1",
            shapes=[mod.RectShape(x1=0.0, y1=0.0, x2=1.84, y2=0.05)],
        )]),
        **{"class": ["CORE"]},
    )
    dumped = macro.model_dump(exclude_none=True, by_alias=True)
    assert dumped["obs"]["type"] == "Obs"
    assert dumped["obs"]["layer_groups"][0]["layer"] == "met1"
    assert dumped["obs"]["layer_groups"][0]["shapes"][0]["type"] == "Rect"


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
