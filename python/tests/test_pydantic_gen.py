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
        sites=[mod.MacroSiteRef(name="core")],
        origin=mod.OriginCmd(x=0, y=0),
        foreigns=[mod.MacroForeign(cell="INV_X1", x=0, y=0)],
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
        obses=[mod.MacroObs(layer_groups=[mod.LayerGroup(
            layer="met1",
            shapes=[mod.RectShape(x1=0.0, y1=0.0, x2=1.84, y2=0.05)],
        )])],
        **{"class": ["CORE"]},
    )
    dumped = macro.model_dump(exclude_none=True, by_alias=True)
    assert dumped["obses"][0]["type"] == "Obs"
    assert dumped["obses"][0]["layer_groups"][0]["layer"] == "met1"
    assert dumped["obses"][0]["layer_groups"][0]["shapes"][0]["type"] == "Rect"


def test_sky130_techlef_parses_losslessly(tmp_path):
    """Real Sky130 tech LEF (792 lines) parses end-to-end and every
    LAYER / VIA / VIARULE block carries its sub-properties in the AST.

    Regression guard: the original `repeat <LAYER_PROPERTY>` form
    silently discarded all LAYER body content (unbound dict-container
    catcher = engine drops the value). Fix wrapped it in a
    `<LAYER_PROPERTIES>:properties=@` array; same for VIA, VIARULE.
    """
    pytest.importorskip("pydantic")
    sky_path = REPO_ROOT / "python" / "tests" / "data" / "sky130hd.tlef"
    g = rawast.Grammar.load(str(GRAMMARS / "lef.rawast"))
    parsed = g.parse_file(str(sky_path))

    # Header sub-statements (Sky130 opens with TIME, not DATABASE —
    # exercises the order-permissive UNITS rewrite).
    units = next(it for it in parsed["items"] if it.get("type") == "Units")
    assert units["time_ns"] == 1
    assert units["database_microns"] == 1000

    # Every LAYER carries its TYPE-discriminated typed fields plus a
    # generic `properties` list for less-common keywords.
    layers = [it for it in parsed["items"] if it.get("type") == "Layer"]
    assert len(layers) >= 10  # nwell, pwell, li1, mcon, met1, …
    li1 = next(la for la in layers if la["name"] == "li1")
    assert li1["layer_type"] == "ROUTING"
    assert li1["direction"] == "VERTICAL"
    assert li1["pitch"] == {"x": 0.46, "y": 0.34}
    assert li1["offset"] == {"x": 0.23, "y": 0.17}
    assert li1["width"] == 0.17
    assert li1["area"] == 0.0561
    assert li1["thickness"] == 0.1
    # Less-common clauses (SPACINGTABLE / RESISTANCE w/qualifier /
    # CAPACITANCE w/qualifier / ANTENNA*) still come through the
    # generic `properties` pass-through.
    kinds = {p["kind"] for p in li1["properties"]}
    assert {"SPACING", "RESISTANCE", "CAPACITANCE", "ANTENNAMODEL"} <= kinds

    # LEF58_TYPE PROPERTY captured (opaque inner string is fine).
    nwell = next(la for la in layers if la["name"] == "nwell")
    assert nwell["layer_type"] == "MASTERSLICE"
    prop58 = next((p for p in nwell["properties"]
                   if p["kind"] == "PROPERTY"
                   and "LEF58_TYPE" in p["values"]), None)
    assert prop58 is not None
    assert "TYPE NWELL ;" in prop58["values"]

    # Every VIA carries its layer-group shapes.
    vias = [it for it in parsed["items"] if it.get("type") == "VIA"]
    assert len(vias) >= 5
    via0 = vias[0]
    assert len(via0["properties"]) >= 1, f"VIA lost properties: {via0}"
    # Each VIA property is either a layer-group dict (with `layer` /
    # `shapes`) or a RESISTANCE dict (`type: "Resistance"`).
    for p in via0["properties"]:
        assert "layer" in p or p.get("type") == "Resistance"

    # VIARULE GENERATE blocks carry their LAYER sections with
    # typed sub-clauses (enclosure / rect / spacing / width_range).
    viarules = [it for it in parsed["items"] if it.get("type") == "VIARULE"]
    assert len(viarules) >= 5
    assert viarules[0].get("layers"), \
        f"VIARULE lost layer sections: {viarules[0]}"

    # The generated Pydantic module imports without error against the
    # new shape (LAYER/VIA/VIARULE now have list[...] properties).
    mod = _generate_lef_models(tmp_path)
    assert hasattr(mod, "Library")
    assert "properties" in mod.LayerBlock.model_fields
    assert "properties" in mod.ViaBlock.model_fields
    # ViaruleBlock now exposes `layers: list[ViaruleLayerSection]`
    # instead of the flat `properties` list (per task #120).
    assert "layers" in mod.ViaruleBlock.model_fields


def test_sky130_sram_macro_parses_losslessly(tmp_path):
    """A real OpenRAM-generated SRAM macro (sky130_sram_1kbyte_1rw1r_8x1024_8)
    exercises a much richer MACRO body than the inline test fixtures:
    51 PINs across IN / OUT / INOUT with bracketed bus names
    (`din0[0]`, `addr0[3]`, …), power pins with USE POWER + SHAPE
    ABUTMENT, multi-layer-group PORTs (power rails span met3 + met4),
    and an OBS section with ~150 RECT shapes across four metal
    layers. Lossless-parse regression guard for MACRO / PIN / PORT /
    OBS / LAYER-group-of-shapes — the patterns that real cell
    libraries lean on.
    """
    pytest.importorskip("pydantic")
    sram_path = REPO_ROOT / "python" / "tests" / "data" / "sky130_sram_1kbyte.lef"
    g = rawast.Grammar.load(str(GRAMMARS / "lef.rawast"))
    parsed = g.parse_file(str(sram_path))

    macros = [it for it in parsed["items"] if it.get("type") == "Macro"]
    assert len(macros) == 1
    macro = macros[0]
    assert macro["name"] == "sky130_sram_1kbyte_1rw1r_8x1024_8"
    assert macro["class"] == ["BLOCK"]
    assert set(macro["symmetry"]) == {"X", "Y", "R90"}

    # All 51 PINs present (8 data-in + 10 addr0 + 10 addr1 + …).
    pins = macro["pins"]
    assert len(pins) >= 50, f"PIN list lost entries: only {len(pins)}"
    pin_names = {p["name"] for p in pins}
    assert "din0[0]" in pin_names  # bracketed identifier round-tripped
    assert "addr0[9]" in pin_names
    assert "clk0" in pin_names
    assert "vccd1" in pin_names

    # Power pin: USE POWER + SHAPE ABUTMENT both catcher-flattened to PIN.
    vccd1 = next(p for p in pins if p["name"] == "vccd1")
    assert vccd1["direction"] == "INOUT"
    assert vccd1["use"] == "POWER"
    assert vccd1["shape"] == "ABUTMENT"
    # Multi-layer-group PORT: vccd1 spans met3 + met4 in 4 layer groups.
    assert len(vccd1["ports"]) == 1
    layer_groups = vccd1["ports"][0]["layer_groups"]
    assert len(layer_groups) >= 4

    # OBS sections captured (now `<MACRO_OBSES>:obses=@` list).
    assert len(macro["obses"]) == 1
    first_obs = macro["obses"][0]
    assert first_obs["type"] == "Obs"
    obs_groups = first_obs["layer_groups"]
    assert len(obs_groups) >= 4  # met1 + met2 + met3 + met4
    total_shapes = sum(len(g["shapes"]) for g in obs_groups)
    assert total_shapes >= 100, f"OBS lost shapes: only {total_shapes}"

    # Generated Pydantic validates against the parsed dict (no extra
    # fields rejected by extra="forbid").
    mod = _generate_lef_models(tmp_path)
    model = mod.Library.model_validate(parsed)
    assert model is not None


def test_sky130_io_pad_parses_losslessly(tmp_path):
    """A real Sky130 IO pad (`sky130_fd_io__top_xres4v2`, 4778 lines)
    exercises two patterns the cell-library SRAM didn't touch:

      * `MACRO ... SOURCE USER ;` — pre-5.6 spec clause, dropped from
        5.7 but still emitted by real PDKs (Sky130 IO is LEF 5.5);
        accepted as a permissive deprecated-form pass-through.
      * PIN-level ANTENNA clauses (15 in this file). Each PIN carries
        them in an `antennas` list via the engine's `[]` list-append
        binding (`<PIN_ANTENNA_PROP>:antennas[]=@`); multiple ANTENNA
        clauses of any kind round-trip losslessly.
    """
    pytest.importorskip("pydantic")
    io_path = REPO_ROOT / "python" / "tests" / "data" / "sky130_fd_io_top_xres4v2.lef"
    g = rawast.Grammar.load(str(GRAMMARS / "lef.rawast"))
    parsed = g.parse_file(str(io_path))

    macros = [it for it in parsed["items"] if it.get("type") == "Macro"]
    assert len(macros) == 1
    macro = macros[0]
    assert macro["name"] == "sky130_fd_io__top_xres4v2"
    assert macro["class"] == ["PAD"]
    # SOURCE is the deprecated 5.6-era field that triggered the parse fix.
    assert macro["source"] == "USER"

    # 25 PINs, 15 carry at least one ANTENNA clause.
    pins = macro["pins"]
    assert len(pins) == 25
    with_antenna = [p for p in pins if p.get("antennas")]
    assert len(with_antenna) == 15, \
        f"ANTENNA captures lost — expected 15, got {len(with_antenna)}"

    amux = next(p for p in pins if p["name"] == "AMUXBUS_A")
    assert amux["direction"] == "INOUT"
    assert amux["use"] == "SIGNAL"
    assert amux["antennas"] == [
        {"kind": "PartialMetalSideArea", "value": 111.168},
    ]

    # OBS section survives (now a list of OBS sections).
    assert len(macro["obses"]) == 1
    assert macro["obses"][0]["type"] == "Obs"
    assert macro["obses"][0]["layer_groups"]

    # Generated Pydantic validates the whole file under extra="forbid".
    mod = _generate_lef_models(tmp_path)
    model = mod.Library.model_validate(parsed)
    assert model.items[0].source == "USER"
    assert isinstance(model.items[0].pins[0].antennas, list)


def test_lef_spec_coverage_phase1(tmp_path):
    """Synthetic spec-coverage LEF — phase 1 covers header (every
    library-level sub-statement), UNITS (all 8), MANUFACTURINGGRID,
    USEMINSPACING, CLEARANCEMEASURE, FIXEDMASK, PROPERTYDEFINITIONS
    (every object × type × RANGE), SITE with ROWPATTERN.

    Phases 2-7 (LAYER full coverage, VIA/VIARULE/NDR, MACRO body,
    PIN, PORT/OBS geometries, BEGINEXT/MAXVIASTACK) extend this file
    incrementally.
    """
    pytest.importorskip("pydantic")
    spec_path = REPO_ROOT / "python" / "tests" / "data" / "lef_spec_coverage.lef"
    g = rawast.Grammar.load(str(GRAMMARS / "lef.rawast"))
    parsed = g.parse_file(str(spec_path))

    # Header — every sub-statement captured.
    hdr = parsed["hdr"]
    assert hdr["version"] == "5.8"
    assert hdr["names_case_sensitive"] is True
    assert hdr["no_wire_extension_at_pin"] is True
    assert hdr["bus_bit_chars"] == "[]"
    assert hdr["divider"] == "/"

    items = parsed["items"]
    by_type = {it["type"]: it for it in items if "type" in it
               and not isinstance(it.get("name"), str)}  # singletons

    # UNITS — every spec sub-statement present.
    units = by_type["Units"]
    for field in ("time_ns", "capacitance_pf", "resistance_ohms",
                  "power_mw", "current_ma", "voltage_v",
                  "database_microns", "frequency_mhz"):
        assert field in units, f"UNITS missing {field}"

    # Singleton library-level commands.
    assert by_type["ManufacturingGrid"]["value"] == 0.005
    assert by_type["ClearanceMeasure"]["value"] == "EUCLIDEAN"
    assert by_type["FixedMask"]["is_fixed_mask"] is True

    # USEMINSPACING appears twice (OBS, PIN) — find both via item list.
    ums = [it for it in items if it.get("type") == "UseMinSpacing"]
    assert {(u["object"], u["value"]) for u in ums} == {("OBS", "ON"), ("PIN", "OFF")}

    # PROPERTYDEFINITIONS — every object × type × RANGE combination.
    pd = by_type["PropertyDefinitions"]
    objects = {d["object"] for d in pd["definitions"]}
    assert objects == {"LAYER", "LIBRARY", "MACRO", "VIA", "VIARULE",
                       "NONDEFAULTRULE", "PIN"}
    types = {d["data_type"] for d in pd["definitions"]}
    assert types == {"INTEGER", "REAL", "STRING"}
    ranged = next(d for d in pd["definitions"] if d["prop_name"] == "prop_ranged")
    assert ranged["range_min"] == 0.0
    assert ranged["range_max"] == 1.0

    # SITE with every sub-statement.
    site = next(it for it in items if it.get("type") == "Site")
    assert site["name"] == "spec_site"
    assert site["class"] == "CORE"
    assert site["symmetry"] == ["X", "Y", "R90"]
    assert site["width"] == 1.0
    assert site["height"] == 2.5
    assert site["rowpattern"] == [
        {"site": "sub_a", "orient": "North"},
        {"site": "sub_b", "orient": "FlipNorth"},
    ]

    # Phase 2: LAYER blocks of every TYPE — `layer_type` is the
    # spec discriminator; common keywords are typed fields; the
    # less-common ones (RESISTANCE w/qual, CAPACITANCE w/qual,
    # ANTENNA*, SPACING, ENCLOSURE, PROPERTY) come through a
    # generic `properties: list[{kind, values}]` pass-through.
    layers = {it["name"]: it for it in items if it.get("type") == "Layer"}
    assert set(layers) == {"spec_routing", "spec_cut", "spec_masterslice",
                           "spec_overlap", "spec_implant"}

    assert layers["spec_routing"]["layer_type"] == "ROUTING"
    assert layers["spec_routing"]["direction"] == "HORIZONTAL"
    assert layers["spec_routing"]["pitch"] == {"x": 0.2, "y": 0.2}
    assert layers["spec_routing"]["offset"] == {"x": 0.1, "y": 0.1}
    assert layers["spec_routing"]["width"] == 0.1
    assert layers["spec_routing"]["area"] == 0.05
    assert layers["spec_routing"]["thickness"] == 0.1
    assert layers["spec_routing"]["edge_capacitance"] == 4.07e-05
    routing_props = layers["spec_routing"]["properties"]
    routing_kinds = [p["kind"] for p in routing_props]
    # Less-common spec_routing keywords fall through to `properties`.
    for kw in ("RESISTANCE", "CAPACITANCE", "ANTENNAAREARATIO",
               "ANTENNADIFFAREARATIO", "ANTENNACUMAREARATIO",
               "ANTENNAMODEL", "SPACING", "PROPERTY"):
        assert kw in routing_kinds, f"spec_routing missing {kw}"

    assert layers["spec_cut"]["layer_type"] == "CUT"
    assert layers["spec_cut"]["width"] == 0.17
    cut_kinds = [p["kind"] for p in layers["spec_cut"]["properties"]]
    assert cut_kinds == ["SPACING", "ENCLOSURE", "ANTENNAAREARATIO",
                          "ANTENNAMODEL", "PROPERTY"]

    assert layers["spec_masterslice"]["layer_type"] == "MASTERSLICE"
    assert [p["kind"] for p in layers["spec_masterslice"]["properties"]] \
        == ["PROPERTY"]

    assert layers["spec_overlap"]["layer_type"] == "OVERLAP"
    # No LAYER_PROPERTY matches → `properties` key absent
    # (engine list-append creates the list lazily; see ANTENNA case).
    assert "properties" not in layers["spec_overlap"]

    assert layers["spec_implant"]["layer_type"] == "IMPLANT"
    assert layers["spec_implant"]["width"] == 0.18
    assert [p["kind"] for p in layers["spec_implant"]["properties"]] \
        == ["SPACING"]

    # LEF58_TYPE PROPERTY captures the embedded LEF58 string as opaque.
    routing_prop = next(p for p in layers["spec_routing"]["properties"]
                        if p["kind"] == "PROPERTY")
    assert routing_prop["values"] == ["LEF58_TYPE", "TYPE ROUTING ;"]

    # Phase 3: VIA, VIARULE, NONDEFAULTRULE.
    via = next(it for it in items
               if it.get("type") == "VIA" and it.get("name") == "spec_via_geom")
    assert via["is_default"] is True
    # First property is RESISTANCE, then three LAYER groups.
    assert via["properties"][0]["type"] == "Resistance"
    assert via["properties"][0]["value"] == 50
    assert [p.get("layer") for p in via["properties"][1:]] == ["li1", "mcon", "met1"]

    # VIARULE-based VIA form — all sub-clauses become typed fields
    # on the VIA dict (no `properties` list).
    via_vr = next(it for it in items
                  if it.get("type") == "VIA"
                  and it.get("name") == "spec_via_viarule_form")
    assert via_vr["via_rule"] == "spec_viarule_gen"
    assert via_vr["cut_size"] == {"x": 0.17, "y": 0.17}
    assert via_vr["layers"] == {"bot": "li1", "cut": "mcon", "top": "met1"}
    assert via_vr["cut_spacing"] == {"x": 0.19, "y": 0.19}
    assert via_vr["enclosure"] == {"bot_x": 0.04, "bot_y": 0.06,
                                   "top_x": 0.06, "top_y": 0.06}
    assert via_vr["rowcol"] == {"rows": 2, "cols": 3}
    assert via_vr["origin"] == {"x": 0.005, "y": 0.005}
    assert via_vr["offset"] == {"bot_x": 0.01, "bot_y": 0.02,
                                "top_x": 0.03, "top_y": 0.04}
    assert via_vr["pattern"] == "myCutPattern"

    viarules = {it["name"]: it for it in items if it.get("type") == "VIARULE"}
    assert "spec_viarule_pair" in viarules
    assert viarules["spec_viarule_gen"]["is_generate"] is True
    # Each VIARULE body is a list of LAYER sections, each with its
    # name and typed sub-clauses (enclosure / rect / spacing / …).
    # VIARULE rect/polygon sub-clauses reuse the same RECT_SHAPE /
    # POLYGON_SHAPE rules used by PORT/OBS and VIA shapes, so they
    # carry the `type` discriminator alongside their geometry.
    assert viarules["spec_viarule_gen"]["layers"] == [
        {"name": "li1",  "enclosure": {"overhang1": 0,    "overhang2": 0}},
        {"name": "met1", "enclosure": {"overhang1": 0.06, "overhang2": 0.03}},
        {"name": "mcon", "rect": {"type": "Rect",
                                   "x1": -0.085, "y1": -0.085,
                                   "x2": 0.085,  "y2": 0.085}},
    ]
    assert viarules["spec_viarule_pair"]["layers"] == [
        {"name": "li1",  "enclosure": {"overhang1": 0,    "overhang2": 0}},
        {"name": "met1", "enclosure": {"overhang1": 0.06, "overhang2": 0.03}},
        {"name": "mcon", "rect": {"type": "Rect",
                                   "x1": -0.085, "y1": -0.085,
                                   "x2": 0.085,  "y2": 0.085},
         "spacing": {"x": 0.36, "y": 0.36}},
    ]

    ndr = next(it for it in items if it.get("type") == "NonDefaultRule")
    assert ndr["name"] == "spec_ndr"
    prop_types = [p.get("type") for p in ndr["properties"]]
    assert prop_types == ["HardSpacing", "Layer", "Layer", "Via",
                          "ViaRule", "MinCuts"]
    # HARDSPACING marker.
    assert ndr["properties"][0]["is_hard_spacing"] is True
    # Per-LAYER properties catcher-flatten into the LAYER sub-block dict.
    li1_layer = ndr["properties"][1]
    assert li1_layer == {"type": "Layer", "layer": "li1",
                         "width": 0.34, "spacing": 0.34}
    met1_layer = ndr["properties"][2]
    assert met1_layer["diag_spacing"] == 0.5
    assert met1_layer["wire_ext"] == 0.05
    # VIA / VIARULE references and MINCUTS.
    assert ndr["properties"][3] == {"type": "Via", "via": "spec_via_geom"}
    assert ndr["properties"][4] == {"type": "ViaRule",
                                    "viarule": "spec_viarule_gen"}
    assert ndr["properties"][5] == {"type": "MinCuts",
                                    "cut_layer": "mcon", "num_cuts": 2}

    # Phase 4: MACRO with every body sub-statement.
    macro = next(it for it in items
                 if it.get("type") == "Macro" and it.get("name") == "spec_macro")
    assert macro["class"] == ["BLOCK"]
    assert macro["is_fixed_mask"] is True       # FIXEDMASK on MACRO
    assert macro["eeq"] == "spec_macro_eq"       # EEQ reference
    assert macro["source"] == "USER"             # legacy SOURCE clause
    assert macro["foreigns"] == [{"cell": "spec_macro", "x": 0, "y": 0}]
    assert macro["origin"] == {"x": 0, "y": 0}
    # spec-canonical: SITE clauses now land in a list
    assert macro["sites"] == [{"name": "spec_site"}]
    assert macro["width"] == 5.0
    assert macro["height"] == 10.0
    assert macro["symmetry"] == ["X", "Y", "R90"]

    # DENSITY block captured with nested layers + per-layer rects.
    density = macro["density"]
    assert density["type"] == "Density"
    assert len(density["layers"]) == 2
    assert density["layers"][0]["layer"] == "met1"
    assert len(density["layers"][0]["rects"]) == 2
    assert density["layers"][0]["rects"][0]["density"] == 0.4

    # Phase 5: PINs exercising every sub-statement.
    assert len(macro["pins"]) == 2
    vdd = next(p for p in macro["pins"] if p["name"] == "VDD")
    assert vdd["use"] == "POWER"
    assert vdd["shape"] == "ABUTMENT"

    sig_in = next(p for p in macro["pins"] if p["name"] == "SIG_IN")
    assert sig_in["direction"] == "OUTPUT"
    assert sig_in["direction_modifier"] == "TRISTATE"  # OUTPUT TRISTATE
    assert sig_in["use"] == "SIGNAL"
    assert sig_in["shape"] == "FEEDTHRU"
    assert sig_in["taper_rule"] == "tapered"
    assert sig_in["net_expr"] == "net_expr_prop net_default"
    assert sig_in["supply_sensitivity"] == "VDD"
    assert sig_in["ground_sensitivity"] == "VSS"
    assert sig_in["must_join"] == "OTHER_PIN"
    # PIN-level PROPERTY clauses — now a list via the `:properties[]=@`
    # binding, so multiple per-PIN PROPERTYs round-trip losslessly.
    assert sig_in["properties"] == [{"name": "prop_pin_i", "value": 7}]
    # ANTENNA clauses — captured as a list via the engine's `[]`
    # list-append binding. Each entry is a dict with `kind`, `value`,
    # and optional `layer`. Multiple ANTENNAs of any kind round-trip
    # losslessly.
    assert sig_in["antennas"] == [
        {"kind": "GateArea", "value": 0.01},
        {"kind": "DiffArea", "layer": "met1", "value": 0.02},
        {"kind": "PartialMetalArea", "layer": "met1", "value": 0.05},
        {"kind": "Model", "value": "OXIDE1"},
    ]

    # Phase 6: full PORT/OBS layerGeometries.
    sig_port = sig_in["ports"][0]
    assert sig_port["port_class"] == "CORE"   # PORT CLASS modifier
    met1_group = sig_port["layer_groups"][0]
    # LAYER-level modifiers: EXCEPTPGNET / SPACING / DESIGNRULEWIDTH / MASK
    # plus the separate WIDTH statement.
    assert met1_group["except_pg_net"] is True
    assert met1_group["spacing"] == 0.05
    assert met1_group["design_rule_width"] == 0.1
    assert met1_group["mask_num"] == 2
    assert met1_group["width"] == 0.12

    shapes = met1_group["shapes"]
    # Shape list: Rect(MASK), Rect(ITERATE), Polygon, Polygon(MASK),
    # Polygon(ITERATE), Path, Path(MASK), Path(ITERATE),
    # ViaPlacement, ViaPlacement(MASK+ITERATE).
    assert [s["type"] for s in shapes] == [
        "Rect", "Rect", "Polygon", "Polygon", "Polygon",
        "Path", "Path", "Path",
        "ViaPlacement", "ViaPlacement",
    ]
    # RECT with MASK
    assert shapes[0]["mask_num"] == 1
    # RECT with ITERATE + stepPattern captured as nested dict
    assert shapes[1]["is_iterate"] is True
    assert shapes[1]["step_pattern"] == {"step_num_x": 2, "step_num_y": 3,
                                          "step_x": 0.5, "step_y": 0.5}
    # POLYGON point list
    assert shapes[2]["points"] == [5, 0, 6, 0, 6, 1, 5.5, 1.2, 5, 1]
    # Polygon with MASK modifier
    assert shapes[3]["mask_num"] == 2
    # Polygon with ITERATE — step pattern numbers fold into `points`
    # (no delimiter token, documented grammar limitation).
    assert shapes[4]["is_iterate"] is True
    # PATH point list
    assert shapes[5]["points"] == [7, 0, 7, 1, 8, 1, 8, 0]
    # Path with MASK
    assert shapes[6]["mask_num"] == 1
    # Path with ITERATE — same limitation as POLYGON ITERATE.
    assert shapes[7]["is_iterate"] is True
    # VIA placement — first form (no MASK / no ITERATE)
    assert shapes[8]["x"] == 9
    assert shapes[8]["y"] == 0
    assert shapes[8]["via_name"] == "spec_via_geom"
    # VIA placement with MASK + ITERATE — step pattern separated
    # cleanly thanks to the `via_name` identifier delimiter.
    assert shapes[9]["mask_num"] == 1
    assert shapes[9]["is_iterate"] is True
    assert shapes[9]["step_pattern"] == {"step_num_x": 2, "step_num_y": 1,
                                         "step_x": 0.5, "step_y": 0.5}

    # Top-level SPACING block — SAMENET inter-layer rules, second
    # with the STACK qualifier.
    spacing = next(it for it in items if it.get("type") == "Spacing")
    assert spacing["items"] == [
        {"layer1": "met1", "layer2": "met2", "min_spacing": 0.14},
        {"layer1": "met2", "layer2": "met3", "min_spacing": 0.14,
         "is_stack": True},
    ]

    # Phase 7: MAXVIASTACK and BEGINEXT.
    mvs = next(it for it in items if it.get("type") == "MaxViaStack")
    assert mvs["max_stack"] == 4
    assert mvs["range_bot"] == "met1"
    assert mvs["range_top"] == "met5"

    ext = next(it for it in items if it.get("type") == "BeginExt")
    assert ext["name"] == "spec_vendor_ext"
    # Inner content captured opaquely (vendor-DSL).
    assert "vendor_directive option_a = 42 ;" in ext["body"]
    assert "another_vendor_thing" in ext["body"]

    # OBS captured (single OBS, now as the only entry of the obses list).
    assert len(macro["obses"]) == 1
    assert macro["obses"][0]["type"] == "Obs"

    # Trailing PROPERTY clauses captured as a list.
    assert macro["properties"] == [
        {"prop_name": "prop_macro_i", "prop_value": 42},
        {"prop_name": "prop_macro_r", "prop_value": 3.14},
        {"prop_name": "prop_macro_s", "prop_value": "hello"},
    ]

    # Generated Pydantic validates the whole thing under extra="forbid".
    mod = _generate_lef_models(tmp_path)
    model = mod.Library.model_validate(parsed)
    assert model is not None


def test_sky130_multi_antenna_cell_round_trips(tmp_path):
    """A real Sky130 standard cell (sky130_fd_sc_hd__dlymetal6s2s_1)
    whose PIN X carries TWO ANTENNA clauses of different kinds —
    `ANTENNADIFFAREA 0.429` followed by `ANTENNAGATEAREA 0.126`.
    Both must survive in the `antennas` list (engine `:name[]=@`
    list-append). Sibling PINs with zero or one ANTENNA round-trip
    too — the `Optional[list[...]]` shape covers absent / list cases.
    """
    pytest.importorskip("pydantic")
    cell_path = REPO_ROOT / "python" / "tests" / "data" \
        / "sky130_fd_sc_hd_dlymetal6s2s_1.lef"
    g = rawast.Grammar.load(str(GRAMMARS / "lef.rawast"))
    parsed = g.parse_file(str(cell_path))

    macro = next(it for it in parsed["items"]
                 if it.get("type") == "Macro")
    pin_x = next(p for p in macro["pins"] if p["name"] == "X")
    assert pin_x["antennas"] == [
        {"kind": "DiffArea", "value": 0.429},
        {"kind": "GateArea", "value": 0.126},
    ]
    pin_a = next(p for p in macro["pins"] if p["name"] == "A")
    assert pin_a["antennas"] == [{"kind": "GateArea", "value": 0.126}]
    # Power pins have no ANTENNA — `antennas` key absent from parsed.
    vgnd = next(p for p in macro["pins"] if p["name"] == "VGND")
    assert "antennas" not in vgnd

    # Full round-trip through generated Pydantic models.
    mod = _generate_lef_models(tmp_path)
    model = mod.Library.model_validate(parsed)
    dumped = model.model_dump(exclude_none=True, by_alias=True)
    assert dumped == parsed


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
