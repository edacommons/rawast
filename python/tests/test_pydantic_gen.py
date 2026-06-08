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
    model = mod.Lef.model_validate(parsed)
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
    model = mod.Lef.model_validate(parsed)
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
    model = mod.Lef.model_validate(parsed)
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
    # generic `properties` pass-through. Sky130 uses the
    # SPACINGTABLE form (`SPACING TABLE PARALLELRUNLENGTH …`),
    # which now correctly classifies as the SPACINGTABLE keyword
    # (previously misclassified as SPACING via prefix-match — the
    # audit-driven LAYER_KEYWORD reorder fixed that).
    kinds = {p["kind"] for p in li1["properties"]}
    assert {"SPACINGTABLE", "RESISTANCE", "CAPACITANCE", "ANTENNAMODEL"} <= kinds

    # LEF58_TYPE PROPERTY captured as a typed Lef58Property dict
    # via the LAYER_LEF58_PROPERTY rule — distinct from generic
    # `layer.properties` (kind="PROPERTY"). The `name` field
    # holds the LEF58 sub-property (e.g. "TYPE", "SPACING") —
    # the `LEF58_` prefix is implicit in the type discriminator,
    # not duplicated. Inner content stays opaque.
    nwell = next(la for la in layers if la["name"] == "nwell")
    assert nwell["layer_type"] == "MASTERSLICE"
    assert nwell["lef58"] == [
        {"type": "Lef58Property", "name": "TYPE",
         "content": "TYPE NWELL ;"},
    ]
    # Generic `properties` should not also contain the LEF58_*
    # entry — the LEF58 rule wins via predictive PEG before the
    # generic fallback fires.
    assert "properties" not in nwell or not any(
        p.get("kind") == "PROPERTY"
        and "LEF58_TYPE" in p.get("values", [])
        for p in nwell.get("properties", [])
    )

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
    assert hasattr(mod, "Lef")
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
    model = mod.Lef.model_validate(parsed)
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
    model = mod.Lef.model_validate(parsed)
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
    assert hdr["version"] == 5.8     # LEF spec: VERSION is a real number
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

    # PROPERTYDEFINITIONS — every spec object × type × RANGE
    # combination. All 14 LEF/DEF 5.8 §"Property Definitions"
    # scopes (the 7 LEF-canonical ones plus the 7 DEF-context
    # ones since the toolchain shares property declarations
    # across LEF/DEF for round-trip symmetry).
    pd = by_type["PropertyDefinitions"]
    objects = {d["object"] for d in pd["definitions"]}
    assert objects == {"LAYER", "LIBRARY", "MACRO", "VIA", "VIARULE",
                       "NONDEFAULTRULE", "PIN", "COMPONENTPIN",
                       "REGION", "DESIGN", "NET", "SPECIALNET",
                       "ROW", "GROUP"}
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
    # LEF/DEF 5.8 §"Site" trailing PROPERTY clauses — typed
    # Property dicts in `site.properties` list, source order.
    assert site["properties"] == [
        {"type": "Property", "prop_name": "prop_site_i",
         "prop_value": 3},
        {"type": "Property", "prop_name": "prop_site_s",
         "prop_value": "site-string"},
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
    # Includes the spec-completeness antenna keywords added by the
    # audit (ANTENNACUMSIDEAREARATIO, ANTENNACUMDIFFSIDEAREARATIO,
    # ANTENNACUMROUTINGPLUSCUT, ANTENNAAREAMINUSDIFF,
    # ANTENNAAREADIFFREDUCEPWL) plus SPACINGTABLE (must classify as
    # itself, not as SPACING via prefix-match) and the routing MIN*/
    # MAX*/MINSTEP/MINIMUMCUT/WIREEXTENSION cluster.
    for kw in (
            "RESISTANCE", "CAPACITANCE",
            "MINWIDTH", "MAXWIDTH", "MINENCLOSEDAREA", "MINSTEP",
            "MINIMUMCUT", "WIREEXTENSION",
            "ANTENNAAREARATIO", "ANTENNADIFFAREARATIO",
            "ANTENNACUMAREARATIO", "ANTENNACUMDIFFAREARATIO",
            "ANTENNASIDEAREARATIO", "ANTENNADIFFSIDEAREARATIO",
            "ANTENNACUMSIDEAREARATIO", "ANTENNACUMDIFFSIDEAREARATIO",
            "ANTENNAAREAFACTOR", "ANTENNASIDEAREAFACTOR",
            "ANTENNAGATEPLUSDIFF", "ANTENNAAREAMINUSDIFF",
            "ANTENNACUMROUTINGPLUSCUT", "ANTENNAAREADIFFREDUCEPWL",
            "ANTENNAMODEL",
            "SPACING", "SPACINGTABLE"):
        assert kw in routing_kinds, f"spec_routing missing {kw}"
    # SPACINGTABLE MUST classify as itself, not as SPACING via
    # prefix-match — guards against accidental reordering of
    # LAYER_KEYWORD that would re-introduce the prefix hazard.
    st = next(p for p in routing_props if p["kind"] == "SPACINGTABLE")
    assert st["values"][:2] == ["PARALLELRUNLENGTH", 0]
    assert "WIDTH" in st["values"]
    # LEF58_* PROPERTY clauses now route to a separate `lef58`
    # list — distinct from generic `properties` (kind="PROPERTY").
    # The dedicated lef58_name terminal parser discriminates by
    # the LEF58_ prefix, no closed-keyword Choice needed.
    assert layers["spec_routing"]["lef58"] == [
        {"type": "Lef58Property", "name": "TYPE",
         "content": "TYPE ROUTING ;"},
    ]
    # `PROPERTY` should NOT appear in the generic properties list —
    # the only PROPERTY in spec_routing was the LEF58_TYPE one,
    # which routes elsewhere.
    assert "PROPERTY" not in routing_kinds

    assert layers["spec_cut"]["layer_type"] == "CUT"
    assert layers["spec_cut"]["width"] == 0.17
    cut_props = layers["spec_cut"]["properties"]
    cut_kinds = [p["kind"] for p in cut_props]
    # LEF/DEF 5.8 §"Layer (Cut)" — bare SPACING + 4 qualifier
    # variants (LAYER, ADJACENTCUTS, PARALLELOVERLAP, AREA) + 3
    # ENCLOSURE forms (bare, ABOVE, BELOW) + ARRAYSPACING +
    # MINIMUMCUT + antenna. The trailing LEF58_TYPE PROPERTY now
    # routes to `layer.lef58` via the LAYER_LEF58_PROPERTY rule,
    # so it is NOT in cut_kinds anymore.
    assert cut_kinds == [
        "SPACING", "SPACING", "SPACING", "SPACING", "SPACING",
        "ENCLOSURE", "ENCLOSURE", "ENCLOSURE",
        "ARRAYSPACING", "MINIMUMCUT",
        "ANTENNAAREARATIO", "ANTENNAMODEL",
    ]
    assert layers["spec_cut"]["lef58"] == [
        {"type": "Lef58Property", "name": "TYPE",
         "content": "TYPE CUT ;"},
    ]
    # Spot-check the qualifiers landed in the values list in
    # source order — guards against future LAYER_PROPERTY
    # restructuring that flattens or reorders the value tail.
    spacing_props = [p for p in cut_props if p["kind"] == "SPACING"]
    assert spacing_props[1]["values"] == [0.18, "LAYER", "met1"]
    assert spacing_props[2]["values"] == [0.20, "ADJACENTCUTS", 4,
                                           "WITHIN", 0.5]
    assert spacing_props[3]["values"] == [0.22, "PARALLELOVERLAP"]
    assert spacing_props[4]["values"] == [0.24, "AREA", 0.5]
    enclosures = [p for p in cut_props if p["kind"] == "ENCLOSURE"]
    assert enclosures[0]["values"] == [0.06, 0.06]
    assert enclosures[1]["values"] == ["ABOVE", 0.05, 0.07]
    assert enclosures[2]["values"] == ["BELOW", 0.04, 0.05]

    assert layers["spec_masterslice"]["layer_type"] == "MASTERSLICE"
    # Only a single LEF58_TYPE PROPERTY → routes to `lef58`, the
    # generic `properties` list is absent (engine list-append
    # creates it lazily; zero matches → key absent).
    assert "properties" not in layers["spec_masterslice"]
    assert layers["spec_masterslice"]["lef58"] == [
        {"type": "Lef58Property", "name": "TYPE",
         "content": "TYPE NWELL ;"},
    ]

    assert layers["spec_overlap"]["layer_type"] == "OVERLAP"
    # No LAYER_PROPERTY matches → `properties` key absent
    # (engine list-append creates the list lazily; see ANTENNA case).
    assert "properties" not in layers["spec_overlap"]

    assert layers["spec_implant"]["layer_type"] == "IMPLANT"
    assert layers["spec_implant"]["width"] == 0.18
    assert [p["kind"] for p in layers["spec_implant"]["properties"]] \
        == ["SPACING"]

    # (LEF58_TYPE PROPERTY is now captured as a typed Lef58Property
    # dict via `layer.lef58` instead of `layer.properties` — see
    # the spec_routing/spec_cut/spec_masterslice assertions above.
    # The embedded LEF58 content string remains opaque per design.)

    # Phase 3: VIA, VIARULE, NONDEFAULTRULE.
    via = next(it for it in items
               if it.get("type") == "VIA" and it.get("name") == "spec_via_geom")
    assert via["is_default"] is True
    # LEF/DEF 5.8 §"Via" geometry-form body: RESISTANCE (once),
    # 3 LAYER groups, then a trailing PROPERTY clause — all slot
    # into the same `properties` array in source order, each with
    # its own discriminator type (Resistance / no-type LAYER / Property).
    assert via["properties"][0]["type"] == "Resistance"
    assert via["properties"][0]["value"] == 50
    assert [p.get("layer") for p in via["properties"][1:4]] == \
        ["li1", "mcon", "met1"]
    assert via["properties"][4] == {"type": "Property",
                                    "prop_name": "prop_via_i",
                                    "prop_value": 42}

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
    # LEF/DEF 5.8 §"Via Rule" non-GENERATE form allows trailing
    # `VIA viaName ;` lines naming the VIAs the rule produces.
    # The GENERATE form (spec_viarule_gen) doesn't use these.
    assert viarules["spec_viarule_pair"]["vias"] == [
        {"type": "Via", "name": "spec_via_geom"},
        {"type": "Via", "name": "spec_via_viarule_form"},
    ]
    assert "vias" not in viarules["spec_viarule_gen"]

    ndr = next(it for it in items if it.get("type") == "NonDefaultRule")
    assert ndr["name"] == "spec_ndr"
    # LEF/DEF 5.8 §"Non-Default Rule" body items in source order:
    # HardSpacing + 2 Layers + Via + ViaRule + UseVia + UseViaRule
    # + MinCuts + 3 trailing Property + 1 trailing ViaInline.
    prop_types = [p.get("type") for p in ndr["properties"]]
    assert prop_types == ["HardSpacing", "Layer", "Layer", "Via",
                          "ViaRule", "UseVia", "UseViaRule", "MinCuts",
                          "Property", "Property", "Property", "ViaInline"]
    # Inline VIA def (NDR_VIA_INLINE rule) — full VIA block with
    # name + LAYER groups + END name, distinct from NDR_VIA_REF.
    inline_via = ndr["properties"][11]
    assert inline_via["name"] == "spec_ndr_inline_via"
    assert inline_via["end_name"] == "spec_ndr_inline_via"
    assert len(inline_via["properties"]) == 2  # 2 LAYER groups
    # HARDSPACING marker.
    assert ndr["properties"][0]["is_hard_spacing"] is True
    # Per-LAYER properties catcher-flatten into the LAYER sub-block dict.
    li1_layer = ndr["properties"][1]
    assert li1_layer == {"type": "Layer", "layer": "li1",
                         "width": 0.34, "spacing": 0.34}
    met1_layer = ndr["properties"][2]
    assert met1_layer["diag_spacing"] == 0.5
    assert met1_layer["wire_ext"] == 0.05
    # VIA / VIARULE references, USEVIA / USEVIARULE, MINCUTS, then
    # trailing Property clauses with INTEGER / REAL / STRING values.
    assert ndr["properties"][3] == {"type": "Via", "via": "spec_via_geom"}
    assert ndr["properties"][4] == {"type": "ViaRule",
                                    "viarule": "spec_viarule_gen"}
    assert ndr["properties"][5] == {"type": "UseVia",
                                    "via": "spec_via_geom"}
    assert ndr["properties"][6] == {"type": "UseViaRule",
                                    "viarule": "spec_viarule_gen"}
    assert ndr["properties"][7] == {"type": "MinCuts",
                                    "cut_layer": "mcon", "num_cuts": 2}
    # Trailing PROPERTY clauses — INTEGER / REAL / STRING values.
    assert ndr["properties"][8] == {"type": "Property",
                                    "prop_name": "prop_ndr_i",
                                    "prop_value": 7}
    assert ndr["properties"][9]["prop_value"] == 1.5
    assert ndr["properties"][10]["prop_value"] == "ndr-string-value"

    # Phase 4: MACRO with every body sub-statement.
    macro = next(it for it in items
                 if it.get("type") == "Macro" and it.get("name") == "spec_macro")
    assert macro["class"] == ["BLOCK"]
    assert macro["is_fixed_mask"] is True       # FIXEDMASK on MACRO
    assert macro["eeq"] == "spec_macro_eq"       # EEQ reference
    assert macro["source"] == "USER"             # legacy SOURCE clause
    # LEF/DEF 5.8 §"Macro" FOREIGN allows both `cell xy` and
    # `cell xy orient` forms; orient is captured as optional field.
    assert macro["foreigns"] == [
        {"cell": "spec_macro", "x": 0, "y": 0},
        {"cell": "spec_macro_mirrored", "x": 0, "y": 0, "orient": "FN"},
    ]
    assert macro["origin"] == {"x": 0, "y": 0}
    # spec-canonical: SITE clauses now land in a list
    assert macro["sites"] == [{"name": "spec_site"}]
    assert macro["width"] == 5.0
    assert macro["height"] == 10.0
    assert macro["symmetry"] == ["X", "Y", "R90"]

    # DENSITY block captured with nested layers + per-layer rects.
    # Each rect carries `type: "DensityRect"` (not "Rect") so that
    # downstream discriminated unions distinguish them from bare
    # RECT_SHAPE shapes.
    density = macro["density"]
    assert density["type"] == "Density"
    assert len(density["layers"]) == 2
    assert density["layers"][0]["layer"] == "met1"
    assert len(density["layers"][0]["rects"]) == 2
    assert density["layers"][0]["rects"][0]["type"] == "DensityRect"
    assert density["layers"][0]["rects"][0]["density"] == 0.4

    # Phase 5: PINs exercising every sub-statement. VDD + SIG_IN
    # + MULTI_PORT_PIN (the latter exercises the spec's "two or
    # more PORTs per pin" form).
    assert len(macro["pins"]) == 3
    vdd = next(p for p in macro["pins"] if p["name"] == "VDD")
    assert vdd["use"] == "POWER"
    assert vdd["shape"] == "ABUTMENT"

    multi = next(p for p in macro["pins"] if p["name"] == "MULTI_PORT_PIN")
    assert len(multi["ports"]) == 2
    assert multi["ports"][0]["port_class"] == "CORE"
    assert multi["ports"][1]["port_class"] == "BUMP"

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
    # PIN-scope LEF58_* PROPERTY clauses route to `pin.lef58` via
    # the PIN_LEF58_PROPERTY rule, distinct from generic
    # pin.properties. Suffix-only name; prefix encoded in the
    # type discriminator.
    assert sig_in["lef58"] == [
        {"type": "Lef58Property", "name": "VOLTAGE",
         "content": "VDD"},
        {"type": "Lef58Property", "name": "DRIVE",
         "content": "STRONG"},
    ]
    # ANTENNA clauses — captured as a list via the engine's `[]`
    # list-append binding. Each entry is a dict with `kind`, `value`,
    # and optional `layer`. Multiple ANTENNAs of any kind round-trip
    # losslessly.
    # Every PIN ANTENNA kind defined by LEF/DEF 5.8 §"Pin" —
    # 10 entries, ordered the same as the source LEF. The
    # ANTENNAANYDIFFAREA / PARTIALMETALSIDEAREA / PARTIALCUTAREA
    # / MAX{,SIDE,CUT}AREACAR entries exercise PIN_ANTENNA_KIND
    # alternatives the IO-pad fixture (`top_xres4v2`) hit only
    # one of (PartialMetalSideArea). Order of the list matters
    # because the grammar preserves source order.
    assert sig_in["antennas"] == [
        {"kind": "GateArea", "value": 0.01},
        {"kind": "DiffArea", "layer": "met1", "value": 0.02},
        {"kind": "PartialMetalArea", "layer": "met1", "value": 0.05},
        {"kind": "PartialMetalSideArea", "layer": "met1", "value": 0.04},
        {"kind": "PartialCutArea", "layer": "mcon", "value": 0.06},
        {"kind": "MaxAreaCar", "layer": "met1", "value": 0.10},
        {"kind": "MaxSideAreaCar", "layer": "met1", "value": 0.08},
        {"kind": "MaxCutCar", "layer": "mcon", "value": 0.12},
        {"kind": "AnyDiffArea", "layer": "met1", "value": 0.03},
        # Deprecated LEF 5.4-era PIN ANTENNA forms — distinct
        # InputGateAreaLegacy / InputDiffAreaLegacy / etc. kinds
        # so downstream consumers can flag them as deprecated.
        {"kind": "InputGateAreaLegacy", "value": 0.05},
        {"kind": "InputDiffAreaLegacy", "layer": "met1", "value": 0.06},
        {"kind": "OutputDiffAreaLegacy", "layer": "met1", "value": 0.07},
        {"kind": "InOutDiffAreaLegacy", "layer": "met1", "value": 0.08},
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

    # Trailing PROPERTY clauses captured as a list. Generic
    # PROPERTY clauses land in `macro.properties`; LEF58_* ones
    # route to `macro.lef58` instead via MACRO_LEF58_PROPERTY +
    # MACRO_TRAILING_PROPERTY (the trailing-position Choice that
    # tries the LEF58 form before the generic KV form).
    assert macro["properties"] == [
        {"prop_name": "prop_macro_i", "prop_value": 42},
        {"prop_name": "prop_macro_r", "prop_value": 3.14},
        {"prop_name": "prop_macro_s", "prop_value": "hello"},
    ]
    assert macro["lef58"] == [
        {"type": "Lef58Property", "name": "PWRPLUS",
         "content": "VDD"},
        {"type": "Lef58Property", "name": "LIBRARYTYPE",
         "content": "STDCELL"},
    ]

    # Generated Pydantic validates the whole thing under extra="forbid".
    mod = _generate_lef_models(tmp_path)
    model = mod.Lef.model_validate(parsed)
    assert model is not None


def test_lef_spec_coverage_save_round_trip(tmp_path):
    """End-to-end save round-trip on the synthetic full-spec LEF.

    parse → save → reparse → assert the second parse equals the
    first. Covers every grammar primitive exercised by the fixture
    in a single sweep: `:name[]=@` list-append on LAYER properties
    / MACRO foreigns / MACRO sites / PIN antennas / PIN properties /
    MACRO OBS list, per-TYPE LAYER typed fields, both VIA forms
    (geometry + VIARULE-based with nested cut_size/layers/…), both
    VIARULE forms with LAYER sections, MACRO body sub-clauses in
    arbitrary order, DENSITY blocks with the DensityRect
    discriminator, BEGINEXT body via the `*` raw-consume
    primitive, PIN body sub-statements (DIRECTION / USE / SHAPE /
    NETEXPR / SUPPLYSENSITIVITY / GROUNDSENSITIVITY / MUSTJOIN /
    PROPERTY / ANTENNA*), PORT/OBS layerGeometries with MASK /
    ITERATE / STEP_PATTERN, and all the structural-keyword
    clauses around them. Regression catcher for any future grammar
    or engine edit that breaks save symmetry.
    """
    spec_path = REPO_ROOT / "python" / "tests" / "data" / "lef_spec_coverage.lef"
    g = rawast.Grammar.load(str(GRAMMARS / "lef.rawast"))

    parsed_a = g.parse_file(str(spec_path))
    saved = g.save(parsed_a)
    text = saved.decode("utf-8") if isinstance(saved, bytes) else saved
    parsed_b = g.parse_string(text)

    assert parsed_b == parsed_a, (
        "spec-coverage LEF lost structure on save round-trip; "
        "the diff between the two parses indicates a save-side gap "
        "in some grammar rule"
    )


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
    model = mod.Lef.model_validate(parsed)
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


def test_def_prelude_parses_and_saves_round_trip(tmp_path):
    """The lef.rawast grammar carries a second top rule `DEF` reachable
    via `start="DEF"` on parse and save. Initial coverage: the file
    prelude (VERSION / BUSBITCHARS / DIVIDERCHAR), DESIGN, UNITS
    DISTANCE MICRONS, DIEAREA, ROW (with DO/BY/STEP), and END
    DESIGN. Exercises the save-side `start=` parameter added so a
    single grammar with multiple top rules can save against any of
    them."""
    g = rawast.Grammar.load(str(GRAMMARS / "lef.rawast"))
    src = (
        "VERSION 5.8 ;\n"
        'BUSBITCHARS "[]" ;\n'
        'DIVIDERCHAR "/" ;\n'
        "DESIGN test_design ;\n"
        "UNITS DISTANCE MICRONS 1000 ;\n"
        "DIEAREA ( 0 0 ) ( 200000 200000 ) ;\n"
        "ROW ROW_0 core 0 0 N DO 78 BY 1 STEP 2540 0 ;\n"
        "ROW ROW_1 core 0 2540 N DO 78 BY 1 STEP 2540 0 ;\n"
        "END DESIGN\n"
    )
    ast = g.parse_string(src, start="DEF")
    assert ast["design"]["name"] == "test_design"
    assert ast["design"]["type"] == "Design"

    units = next(it for it in ast["items"] if it.get("type") == "Units")
    assert units["database_microns"] == 1000

    diearea = next(it for it in ast["items"] if it.get("type") == "DieArea")
    assert diearea["points"] == [
        {"x": 0, "y": 0},
        {"x": 200000, "y": 200000},
    ]

    rows = [it for it in ast["items"] if it.get("type") == "Row"]
    assert len(rows) == 2
    assert rows[0]["name"] == "ROW_0"
    assert rows[0]["site"] == "core"
    assert rows[0]["orient"] == "N"
    assert rows[0]["num_x"] == 78
    assert rows[0]["step_x"] == 2540

    # Save round-trip through the new start= parameter — saves
    # against `DEF` instead of the grammar's default `LEF` top rule.
    saved = g.save(ast, start="DEF").decode("utf-8")
    ast2 = g.parse_string(saved, start="DEF")
    assert ast == ast2


def test_def_spec_coverage_phase1(tmp_path):
    """`python/tests/data/def_spec_coverage.def` — synthetic DEF
    fixture exercising every Phase-1 clause (header + DESIGN + UNITS
    + DIEAREA + ROW with all 8 spec orientations + END DESIGN).
    Grows commit-by-commit as more DEF grammar lands; the file's
    header comment lists the planned phases.

    Asserts:
      * every field of every captured clause has the spec'd type
        and value;
      * parse → save → reparse on the same fixture is
        structurally equivalent (save's new `start="DEF"` parameter);
      * the generated Pydantic `Def` class validates the parsed
        dict and round-trips through `model_dump()`.
    """
    g = rawast.Grammar.load(str(GRAMMARS / "lef.rawast"))
    p = REPO_ROOT / "python/tests/data/def_spec_coverage.def"
    parsed = g.parse_file(str(p), start="DEF")

    # Header — VERSION as <NUMBER>, DIVIDERCHAR + BUSBITCHARS as
    # string-quoted single chars or pairs.
    hdr = parsed["hdr"]
    assert hdr["version"] == 5.8
    assert hdr["divider"] == "/"
    assert hdr["bus_bit_chars"] == "[]"

    # DESIGN
    assert parsed["design"]["type"] == "Design"
    assert parsed["design"]["name"] == "spec_design"

    # Phase 2: TECHNOLOGY + HISTORY + PROPERTYDEFINITIONS.
    tech = next(it for it in parsed["items"]
                if it.get("type") == "Technology")
    assert tech["name"] == "my_tech"

    histories = [it for it in parsed["items"]
                 if it.get("type") == "History"]
    assert len(histories) == 3
    # `*` raw-consume captures everything between the keyword and
    # the terminating `;`, preserving embedded spaces verbatim.
    assert histories[0]["text"] == " Created on 2026-06-08 by toolchain v1.0 "
    assert histories[2]["text"] == " Final placement verified "

    pd = next(it for it in parsed["items"]
              if it.get("type") == "PropertyDefinitions")
    pd_objects = {d["object"] for d in pd["definitions"]}
    # DEF-context scopes per LEF/DEF 5.8 §"Property Definitions"
    # — DEF can declare properties for every spec scope including
    # the LEF-only ones (a DEF often references a LEF library).
    assert {"DESIGN", "NET", "SPECIALNET", "COMPONENTPIN",
            "REGION", "ROW", "GROUP", "NONDEFAULTRULE"} <= pd_objects
    # RANGE and explicit default values both captured.
    ranged = next(d for d in pd["definitions"]
                  if d["prop_name"] == "prop_ndr_r")
    assert ranged["range_min"] == 0.0 and ranged["range_max"] == 10.0
    int_def = next(d for d in pd["definitions"]
                   if d["prop_name"] == "prop_d_def_i")
    assert int_def["default_value"] == 7
    str_def = next(d for d in pd["definitions"]
                   if d["prop_name"] == "prop_n_def_s")
    assert str_def["default_value"] == "default-net-attr"

    # UNITS DISTANCE MICRONS — DEF's UNITS is the single-line form,
    # distinct from LEF's UNITS_BLOCK.
    units = next(it for it in parsed["items"] if it.get("type") == "Units")
    assert units["database_microns"] == 2000

    # DIEAREA — N-point form per LEF/DEF 5.8 §"DieArea". Two
    # points define a rectangle; more points define a die
    # polygon outline. The fixture uses the 2-point form.
    die = next(it for it in parsed["items"] if it.get("type") == "DieArea")
    assert die["points"] == [
        {"x": -100, "y": -100},
        {"x": 100100, "y": 100100},
    ]

    # ROW — eight spec orientations (N S E W FN FS FE FW), one per
    # ROW in the fixture. Verify every captured field per ROW.
    rows = [it for it in parsed["items"] if it.get("type") == "Row"]
    assert len(rows) == 8
    orients = [r["orient"] for r in rows]
    assert orients == ["N", "S", "E", "W", "FN", "FS", "FE", "FW"]
    # Every row references the same site
    assert all(r["site"] == "spec_site" for r in rows)
    # Numeric fields are real numbers, not strings
    for r in rows:
        assert isinstance(r["x"], (int, float))
        assert isinstance(r["y"], (int, float))
        assert isinstance(r["num_x"], int)
        assert isinstance(r["num_y"], int)
        assert isinstance(r["step_x"], int)
        assert isinstance(r["step_y"], int)
    # Row 0: N orient, 10×1 grid stepping in x
    assert (rows[0]["num_x"], rows[0]["num_y"]) == (10, 1)
    assert (rows[0]["step_x"], rows[0]["step_y"]) == (2540, 0)
    # Row 2: E orient, 1×8 grid stepping in y (vertical column)
    assert (rows[2]["num_x"], rows[2]["num_y"]) == (1, 8)
    assert (rows[2]["step_x"], rows[2]["step_y"]) == (0, 1900)

    # Phase 3: TRACKS + GCELLGRID + VIAS.
    tracks = [it for it in parsed["items"] if it.get("type") == "Tracks"]
    assert len(tracks) == 3
    assert tracks[0]["direction"] == "X"
    assert tracks[0]["layers"] == ["met1"]
    # MASK qualifier — captured as optional field. SAMEMASK is a
    # separate boolean flag on top of MASK.
    assert tracks[1]["mask"] == 2
    assert tracks[1]["layers"] == ["met2", "met3"]
    assert "same_mask" not in tracks[1]
    assert tracks[2]["mask"] == 3
    assert tracks[2]["same_mask"] is True

    gcellgrids = [it for it in parsed["items"]
                  if it.get("type") == "GCellGrid"]
    assert len(gcellgrids) == 2
    assert gcellgrids[0] == {"type": "GCellGrid", "direction": "X",
                             "start": 0, "num_gcells": 11, "step": 1000}
    assert gcellgrids[1]["direction"] == "Y"

    # VIAS block — 3 entries exercising geometry-form, VIARULE-
    # form, and POLYGON geometry. Each entry's `clauses` list
    # holds typed dicts in source order.
    vias = next(it for it in parsed["items"] if it.get("type") == "Vias")
    assert vias["count"] == 3
    assert len(vias["vias"]) == 3
    geom = vias["vias"][0]
    assert geom["name"] == "spec_via_geom"
    assert len(geom["clauses"]) == 2
    assert geom["clauses"][0] == {
        "type": "Rect", "layer": "met1",
        "x1": -100, "y1": -100, "x2": 100, "y2": 100,
    }
    assert geom["clauses"][1]["mask"] == 2
    vr = vias["vias"][1]
    assert vr["name"] == "spec_via_rule"
    clause_types = [c["type"] for c in vr["clauses"]]
    assert clause_types == ["ViaRule", "CutSize", "Layers", "CutSpacing",
                            "Enclosure", "RowCol", "Origin", "Offset",
                            "Pattern"]
    poly = vias["vias"][2]
    assert poly["name"] == "spec_via_polygon"
    assert [c["type"] for c in poly["clauses"]] == ["Polygon", "Polygon"]
    # 4-vertex polygon's points captured as a list of {x, y} dicts.
    assert poly["clauses"][0]["points"] == [
        {"x": 0,   "y": 0},   {"x": 100, "y": 0},
        {"x": 100, "y": 100}, {"x": 0,   "y": 100},
    ]
    assert poly["clauses"][1]["mask"] == 1

    # Phase 4: NONDEFAULTRULES + REGIONS + COMPONENTMASKSHIFT.
    cms = next(it for it in parsed["items"]
               if it.get("type") == "ComponentMaskShift")
    assert cms["layers"] == ["met1", "met2", "met3"]

    ndrs = next(it for it in parsed["items"]
                if it.get("type") == "NonDefaultRules")
    assert ndrs["count"] == 2
    assert len(ndrs["rules"]) == 2
    full = ndrs["rules"][0]
    assert full["name"] == "spec_ndr_full"
    assert [c["type"] for c in full["clauses"]] == [
        "HardSpacing", "Layer", "Layer", "Via", "ViaRule",
        "MinCuts", "Property",
    ]
    # First Layer has WIREEXT + SPACING; second has DIAGWIDTH +
    # SPACING. The optional fields land directly on the clause
    # dict via DEF_NDR_LAYER_*-flavour sub-rules' catcher binds.
    assert full["clauses"][1] == {
        "type": "Layer", "layer": "met1",
        "width": 0.2, "spacing": 0.2, "wire_ext": 0.05,
    }
    assert full["clauses"][2]["diag_width"] == 0.4
    minimal = ndrs["rules"][1]
    assert minimal["name"] == "spec_ndr_minimal"
    assert len(minimal["clauses"]) == 1

    regions = next(it for it in parsed["items"]
                   if it.get("type") == "Regions")
    assert regions["count"] == 2
    fence = regions["regions"][0]
    assert fence["name"] == "spec_region_fence"
    # Single box; `boxes` is the list of `( pt pt )` pairs.
    assert fence["boxes"] == [
        {"x1": 0, "y1": 0, "x2": 100, "y2": 100},
    ]
    # TYPE FENCE then PROPERTY trailer.
    assert [c["type"] for c in fence["clauses"]] == ["Type", "Property"]
    assert fence["clauses"][0]["region_type"] == "FENCE"
    guide = regions["regions"][1]
    assert guide["name"] == "spec_region_guide"
    assert len(guide["boxes"]) == 2  # Multi-box region.
    assert guide["clauses"][0]["region_type"] == "GUIDE"

    # Phase 5: COMPONENTS. Four entries exercising every spec
    # clause variant.
    comps = next(it for it in parsed["items"]
                 if it.get("type") == "Components")
    assert comps["count"] == 4
    assert len(comps["components"]) == 4

    fixed = comps["components"][0]
    assert fixed["name"] == "spec_comp_fixed"
    assert fixed["model"] == "cellModelA"
    assert fixed["clauses"] == [
        {"type": "Fixed", "x": 100, "y": 200, "orient": "N"},
    ]

    full = comps["components"][1]
    assert full["name"] == "spec_comp_full"
    # All 9 typed clauses in source order.
    assert [c["type"] for c in full["clauses"]] == [
        "EeqMaster", "Source", "Placed", "Halo", "RouteHalo",
        "Weight", "Region", "MaskShift", "Property",
    ]
    # Spot-check non-trivial fields.
    halo = next(c for c in full["clauses"] if c["type"] == "Halo")
    assert (halo["left"], halo["bottom"], halo["right"],
            halo["top"]) == (10, 20, 30, 40)
    assert "is_soft" not in halo
    rh = next(c for c in full["clauses"] if c["type"] == "RouteHalo")
    assert rh == {"type": "RouteHalo", "halo_dist": 50,
                  "min_layer": "met1", "max_layer": "met3"}
    placed = next(c for c in full["clauses"] if c["type"] == "Placed")
    assert placed["orient"] == "FS"

    cover = comps["components"][2]
    assert cover["clauses"][0] == {
        "type": "Cover", "x": 0, "y": 0, "orient": "W",
    }
    # HALO SOFT — optional `is_soft` flag set.
    cover_halo = cover["clauses"][1]
    assert cover_halo["type"] == "Halo"
    assert cover_halo["is_soft"] is True

    unplaced = comps["components"][3]
    assert unplaced["clauses"] == [{"type": "Unplaced"}]

    # Phase 6: PINS. Three entries cover every clause variant.
    pins_block = next(it for it in parsed["items"]
                      if it.get("type") == "Pins")
    assert pins_block["count"] == 3
    assert len(pins_block["pins"]) == 3

    clk = pins_block["pins"][0]
    assert clk["name"] == "spec_pin_clk"
    clk_types = [c["type"] for c in clk["clauses"]]
    assert clk_types == [
        "Net", "Direction", "Use", "NetExpr",
        "AntennaPinGateArea", "AntennaPinPartialMetalArea",
        "AntennaModel", "Layer", "Placed",
    ]
    # NetExpr keeps the spec's quoted-string content verbatim.
    netexpr = next(c for c in clk["clauses"] if c["type"] == "NetExpr")
    assert netexpr["expr"] == "(net_expr clk_net)"

    vdd = pins_block["pins"][1]
    assert vdd["name"] == "spec_pin_vdd"
    # SPECIAL is a boolean flag — `is_special: True`.
    special = next(c for c in vdd["clauses"] if c["type"] == "Special")
    assert special["is_special"] is True
    # Every typed ANTENNAPIN kind is exercised.
    antenna_types = {c["type"] for c in vdd["clauses"]
                     if c["type"].startswith("Antenna")}
    assert antenna_types == {
        "AntennaPinDiffArea",
        "AntennaPinPartialMetalSideArea",
        "AntennaPinPartialCutArea",
        "AntennaPinMaxAreaCar",
        "AntennaPinMaxSideAreaCar",
        "AntennaPinMaxCutCar",
    }
    # LAYER with MASK + SPACING qualifier.
    layer_c = next(c for c in vdd["clauses"] if c["type"] == "Layer")
    assert layer_c["layer"] == "met2"
    assert layer_c["mask"] == 1
    assert layer_c["spacing"] == 0.05
    assert (layer_c["x1"], layer_c["y1"]) == (100, 100)
    # POLYGON with MASK + DESIGNRULEWIDTH qualifier and 4 points.
    poly_c = next(c for c in vdd["clauses"] if c["type"] == "Polygon")
    assert poly_c["mask"] == 2
    assert poly_c["design_rule_width"] == 0.1
    assert len(poly_c["points"]) == 4
    # VIA placement with MASK.
    via_c = next(c for c in vdd["clauses"] if c["type"] == "Via")
    assert via_c == {"type": "Via", "via": "spec_via_geom",
                     "mask": 1, "x": 150, "y": 150}
    # FIXED placement at the pin level.
    fixed_c = next(c for c in vdd["clauses"] if c["type"] == "Fixed")
    assert fixed_c["orient"] == "N"

    free = pins_block["pins"][2]
    assert free["name"] == "spec_pin_free"
    assert [c["type"] for c in free["clauses"]] == [
        "Net", "Direction", "Unplaced",
    ]

    # Phase 7: PINPROPERTIES + BLOCKAGES + SLOTS.
    pps = next(it for it in parsed["items"]
               if it.get("type") == "PinProperties")
    assert pps["count"] == 3
    assert [e["type"] for e in pps["entries"]] == [
        "ComponentPin", "Pin", "ComponentPin",
    ]
    # First entry: COMPONENT scope with 2 trailing PROPERTYs.
    first = pps["entries"][0]
    assert first["component"] == "spec_comp_full"
    assert first["pin"] == "in1"
    assert len(first["properties"]) == 2
    # Second entry: PIN scope with 1 property.
    pin_entry = pps["entries"][1]
    assert pin_entry["pin"] == "spec_pin_clk"
    assert pin_entry["properties"] == [
        {"type": "Property", "prop_name": "prop_design_i",
         "prop_value": 7},
    ]

    blockages = next(it for it in parsed["items"]
                     if it.get("type") == "Blockages")
    assert blockages["count"] == 3
    # 2 LAYER blockages + 1 PLACEMENT blockage.
    assert [b["type"] for b in blockages["blockages"]] == [
        "LayerBlockage", "LayerBlockage", "PlacementBlockage",
    ]
    # First LAYER blockage: SLOTS + PUSHDOWN + COMPONENT qualifier
    # + RECT + POLYGON shapes.
    lb1 = blockages["blockages"][0]
    assert lb1["layer"] == "met1"
    lb1_types = [c["type"] for c in lb1["clauses"]]
    assert lb1_types == [
        "SlotsFlag", "PushdownFlag", "Component", "Rect", "Polygon",
    ]
    # Second LAYER blockage: 5 qualifier clauses + 1 RECT.
    lb2 = blockages["blockages"][1]
    assert lb2["layer"] == "met2"
    lb2_types = [c["type"] for c in lb2["clauses"]]
    assert lb2_types == [
        "FillsFlag", "ExceptPgNetFlag", "Spacing",
        "DesignRuleWidth", "Mask", "Rect",
    ]
    # PLACEMENT blockage: SOFT + PARTIAL + PUSHDOWN + RECT.
    pb = blockages["blockages"][2]
    pb_types = [c["type"] for c in pb["clauses"]]
    assert pb_types == ["SoftFlag", "Partial", "PushdownFlag", "Rect"]
    partial = next(c for c in pb["clauses"] if c["type"] == "Partial")
    assert partial["max_density"] == 0.5

    slots = next(it for it in parsed["items"]
                 if it.get("type") == "Slots")
    assert slots["count"] == 2
    s1 = slots["slots"][0]
    assert s1["layer"] == "met1"
    assert [sh["type"] for sh in s1["shapes"]] == ["Rect", "Rect"]
    s2 = slots["slots"][1]
    assert s2["layer"] == "met2"
    assert s2["shapes"][0]["type"] == "Polygon"
    assert len(s2["shapes"][0]["points"]) == 4

    # Phase 8: FILLS. Two LAYER entries (one with full qualifier
    # set, one minimal) plus one VIA entry exercising the
    # shift-bitmask MASK form and point placements.
    fills = next(it for it in parsed["items"] if it.get("type") == "Fills")
    assert fills["count"] == 3
    layer_fills = [f for f in fills["fills"]
                   if f["type"] == "LayerFill"]
    via_fills = [f for f in fills["fills"]
                 if f["type"] == "ViaFill"]
    assert len(layer_fills) == 2 and len(via_fills) == 1

    # First LAYER fill: MASK + OPC + RECT + POLYGON.
    lf1 = layer_fills[0]
    assert lf1["layer"] == "met1"
    lf1_types = [c["type"] for c in lf1["clauses"]]
    assert lf1_types == ["Mask", "OpcFlag", "Rect", "Polygon"]
    mask_c = next(c for c in lf1["clauses"] if c["type"] == "Mask")
    assert mask_c["mask"] == 2
    opc_c = next(c for c in lf1["clauses"] if c["type"] == "OpcFlag")
    assert opc_c["is_opc"] is True

    # Minimal LAYER fill — just one RECT, no qualifiers.
    lf2 = layer_fills[1]
    assert lf2["layer"] == "met2"
    assert [c["type"] for c in lf2["clauses"]] == ["Rect"]

    # VIA fill: shift-bitmask MASK (string, not int) + OPC + 3 Points.
    vf = via_fills[0]
    assert vf["via"] == "spec_via_geom"
    vf_types = [c["type"] for c in vf["clauses"]]
    assert vf_types == ["MaskShift", "OpcFlag", "Point", "Point", "Point"]
    mask_shift = next(c for c in vf["clauses"]
                      if c["type"] == "MaskShift")
    assert mask_shift["mask_shift"] == "0xA5"
    points = [c for c in vf["clauses"] if c["type"] == "Point"]
    assert points[0] == {"type": "Point", "x": 100, "y": 100}
    assert points[2] == {"type": "Point", "x": 300, "y": 300}

    # Phase 9: SPECIALNETS + NETS. The two connectivity sections.
    sn = next(it for it in parsed["items"]
              if it.get("type") == "SpecialNets")
    assert sn["count"] == 2
    vdd = sn["nets"][0]
    assert vdd["name"] == "VDD"
    # Three connection forms: PIN, *, comp-pin.
    assert [c["type"] for c in vdd["connections"]] == [
        "Pin", "Global", "CompPin",
    ]
    assert vdd["connections"][2] == {
        "type": "CompPin", "component": "spec_comp_full", "pin": "VDD",
    }
    # Every spec'd SPECIALNET clause type in source order.
    vdd_clauses = [c["type"] for c in vdd["clauses"]]
    assert vdd_clauses == [
        "Use", "Synthesized", "Voltage", "Spacing", "Source",
        "Original", "Pattern", "EstCap", "Weight", "Property",
        "Routed", "Shield",
    ]
    routed = next(c for c in vdd["clauses"] if c["type"] == "Routed")
    assert routed["layer"] == "met1" and routed["width"] == 100
    # SHIELD routing — extra shield_net field on top of layer + width.
    shield = next(c for c in vdd["clauses"] if c["type"] == "Shield")
    assert shield["shield_net"] == "shield_net"
    assert shield["layer"] == "met2" and shield["width"] == 50

    # VSS exercises FIXED and COVER routing keywords.
    vss = sn["nets"][1]
    vss_clauses = [c["type"] for c in vss["clauses"]]
    assert vss_clauses == [
        "Use", "FixedBump", "FixedRoute", "CoverRoute",
    ]

    nets = next(it for it in parsed["items"]
                if it.get("type") == "Nets")
    assert nets["count"] == 3
    # sig_data: every NETS-specific clause.
    sig_data = nets["nets"][0]
    sig_data_types = [c["type"] for c in sig_data["clauses"]]
    assert sig_data_types == [
        "Use", "Source", "NonDefaultRule", "Frequency", "Original",
        "Pattern", "EstCap", "Weight", "ShieldNet", "SubNet",
        "Xtalk", "Property", "Routed",
    ]
    # Routing qualifiers (SHAPE + MASK + NONDEFAULTRULE) land in
    # `qualifiers[]` on the Routed clause; path elements in `path[]`.
    net_routed = next(c for c in sig_data["clauses"]
                      if c["type"] == "Routed")
    qual_types = [q["type"] for q in net_routed["qualifiers"]]
    assert qual_types == ["Shape", "Mask", "NonDefaultRule"]
    path_types = [p["type"] for p in net_routed["path"]]
    assert path_types == ["Point", "Point", "Via"]

    # sig_clk: NOSHIELD routing.
    sig_clk = nets["nets"][1]
    sig_clk_types = [c["type"] for c in sig_clk["clauses"]]
    assert sig_clk_types == ["Use", "FixedBump", "NoShield"]

    # sig_test: FIXED + COVER NETS routing (distinct from
    # SPECIALNETS FixedRoute/CoverRoute since NETS Routes don't
    # have explicit width).
    sig_test = nets["nets"][2]
    assert [c["type"] for c in sig_test["clauses"]] == [
        "Use", "FixedRoute", "CoverRoute",
    ]

    # Phase 10: SCANCHAINS + GROUPS + BEGINEXT.
    chains = next(it for it in parsed["items"]
                  if it.get("type") == "ScanChains")
    assert chains["count"] == 2
    full_chain = chains["chains"][0]
    assert full_chain["name"] == "chain_full"
    full_clause_types = [c["type"] for c in full_chain["clauses"]]
    assert full_clause_types == [
        "CommonScanPins", "Start", "Floating", "Ordered",
        "Stop", "Partition",
    ]
    # FLOATING entries — each is `compName + optional (IN/OUT/BITS)`.
    floating = next(c for c in full_chain["clauses"]
                    if c["type"] == "Floating")
    assert len(floating["entries"]) == 2
    assert floating["entries"][0]["comp"] == "flop_a"
    # PARTITION with optional MAXBITS.
    partition = next(c for c in full_chain["clauses"]
                     if c["type"] == "Partition")
    assert partition["partition"] == "partition_a"
    assert partition["max_bits"] == 32
    # Minimal chain — just START + STOP.
    minimal_chain = chains["chains"][1]
    assert [c["type"] for c in minimal_chain["clauses"]] == ["Start", "Stop"]

    groups = next(it for it in parsed["items"]
                  if it.get("type") == "Groups")
    assert groups["count"] == 2
    main_group = groups["groups"][0]
    assert main_group["name"] == "cluster_main"
    # Two comp-name patterns BEFORE the `+` clauses.
    assert main_group["patterns"] == ["spec_comp_full", "spec_comp_fixed"]
    # REGION + SOFT + PROPERTY clauses.
    main_types = [c["type"] for c in main_group["clauses"]]
    assert main_types == ["Region", "Soft", "Property"]
    # SOFT carries three positional qualifiers.
    soft = next(c for c in main_group["clauses"] if c["type"] == "Soft")
    soft_quals = [q["type"] for q in soft["quals"]]
    assert soft_quals == ["MaxHalfPerimeter", "MaxX", "MaxY"]

    # BEGINEXT/ENDEXT — reuses the existing LEF rule. Body is
    # captured opaquely via the `*` raw-consume primitive.
    ext = next(it for it in parsed["items"]
               if it.get("type") == "BeginExt")
    assert ext["name"] == "spec_vendor"
    assert "vendor_directive" in ext["body"]

    # Parse → save → reparse round-trip via the new start="DEF"
    # parameter on save (mirrors parse's start=).
    saved = g.save(parsed, start="DEF").decode("utf-8")
    reparsed = g.parse_string(saved, start="DEF")
    assert reparsed == parsed

    # Generated Pydantic model accepts the parsed dict and round-trips
    # through model_dump. `Def` is the class for the DEF top rule.
    pytest.importorskip("pydantic")
    mod = _generate_lef_models(tmp_path)
    assert hasattr(mod, "Def")
    model = mod.Def.model_validate(parsed)
    dumped = model.model_dump(exclude_none=True, by_alias=True)
    assert dumped == parsed
