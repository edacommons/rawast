# Shipped grammars

Six grammars ship with the engine, addressable in Python by short name (`Grammar("json")`, `Grammar("lefdef")`, …) or by path (`Grammar.load("grammars/lefdef.rawast")`). All are MIT-licensed, all are versioned in the repo, and all are exercised by the test suite.

## Format-and-corpus summary

| Short name | File | Format | Corpus | Status |
|---|---|---|---|---|
| `json` | `grammars/json.json` | JSON (RFC 8259) | bootstrap | shipped |
| `rawast` | `grammars/rawast.{rawast,json}` | rawast meta-grammar (self-host) | meta | shipped |
| `gdsii` | `grammars/gdsii.rawast` | GDSII binary IC layout | 1,171 files | shipped |
| `lefdef` | `grammars/lefdef.rawast` | LEF + DEF 5.8 (unified) | 770 files | shipped |
| `tcl` | `grammars/tcl.rawast` | Tcl Tier-1 structural | 1,440 files | shipped |

## `json` — strict JSON

`grammars/json.json`. Full JSON per RFC 8259 with array/dict containers. Uses the `std` parser group, with whitespace in the ignore list attached to the start rule.

The in-memory `make_json_grammar()` C++ helper is a JSONC variant (adds `//` line comments and `/* */` block comments to the ignore list). It's the bootstrap grammar used to read JSON-form grammar files (which typically carry inline doc comments).

## `rawast` — the meta-grammar

`grammars/rawast.rawast` (DSL form) and `grammars/rawast.json` (JSON form, equivalent). The grammar describing the `.rawast` grammar DSL itself.

Self-hosted: load `rawast.rawast` with the runtime meta-grammar, get the same value tree as parsing `rawast.json`. The `test_meta_grammar_design_matches_runtime` test locks this in.

Used to load every other `.rawast` file in the repo, including itself.

## `gdsii` — binary IC layout

`grammars/gdsii.rawast`. All 47 GDSII record types, the seven element kinds (BOUNDARY, PATH, SREF, AREF, TEXT, NODE, BOX), full structural schema.

**Corpus:** parses **1,171 / 1,171** real production GDSII files from open-PDK chip flows (Sky130, GF180MCU, IHP130, asap7, gf180, ihp-sg13g2).

**Save round-trip:** on a 750-file local PDK subset, `parse → save → reparse` is **750 / 750 byte-equivalent**. GDSII's binary layout is fully prescribed, so byte-equivalence is achievable and verified. Synthetic round-trip tests additionally cover byte-identical output including the 2,048-byte alignment padding.

**Parser group:** `gdsii` (47 record-type parsers — `header`, `bgnlib`, …, `endmasks`). Addressable as bare (`header`) or qualified (`gdsii.header`).

## `lefdef` — LEF + DEF 5.8 (unified grammar)

`grammars/lefdef.rawast`. One file, two top rules (`LEF` and `DEF`). Host code passes `start="LEF"` or `start="DEF"` to `parse_*` and `save` to pick the language. Shared sub-rules across both: `PROPERTYDEFINITIONS`, `XY_PAIR`, `PAREN_POINT`, `RECT_COORDS`, `POINT_DICT`, `BEGINEXT_BLOCK`, etc.

### LEF side

Base-spec (LEF/DEF 5.8) coverage, less LEF58_* (deferred to a consumer-supplied sub-grammar — content captured as opaque properties, downstream consumer subparses).

**Captured structure:**
- LAYER blocks expose per-TYPE typed fields (`layer_type`, `direction`, `pitch`, `width`, …) including the full ANTENNA*-family list capture and LEF 5.4-era deprecated antenna forms.
- VIA blocks model both the geometry-form and VIARULE-based forms.
- VIARULE blocks model the LAYER-pair and GENERATE forms with typed sub-clauses (`enclosure`, `rect`, `spacing`).
- PIN / MACRO bodies cover every spec sub-statement including DENSITY, MUSTJOIN, EEQ, FIXEDMASK.
- SITE supports the trailing PROPERTY clauses and ROWPATTERN.

**Corpus:**
- Parses **507 / 507** real LEFs across six PDK / open-platform sources.
- On a 263-file local PDK subset (Sky130 / asap7 / gf130bcd / ihp-sg13g2 / NanGate / gf180) covering both **cell LEFs and tech LEFs**, `parse → save → reparse` is **263 / 263 structurally equivalent**.
- Synthetic full-spec fixture (`python/tests/data/lef_spec_coverage.lef`) exercises every spec clause and round-trips through the Pydantic model generator end-to-end.

### DEF side

Every documented LEF/DEF 5.8 §"DEF File" section is supported: TECHNOLOGY / HISTORY / PROPERTYDEFINITIONS / UNITS / DIEAREA / ROW / TRACKS / GCELLGRID / VIAS / NDRS / REGIONS / COMPONENTMASKSHIFT / COMPONENTS / PINS / PINPROPERTIES / BLOCKAGES / SLOTS / FILLS / STYLES / SPECIALNETS / NETS / SCANCHAINS / GROUPS / BEGINEXT.

Plus every real-world variant surfaced by a local 436-DEF corpus chase: NEW continuation segments, `*` wildcard coords, bare via references, mid-path MASK, PIN `+ PORT` sub-blocks, vendor `+ PATTERNNAME` and many more.

**Corpus:**
- `parse → save → reparse` is **435 / 435 structurally equivalent** on a 435-file local DEF corpus.
- One remaining file (a 100 MB+ ChipFlow output) parses and round-trips cleanly but is skipped from the automated runner for speed.
- Synthetic full-spec fixture (`python/tests/data/def_spec_coverage.def`) exercises every documented section + every real-world variant from the corpus chase.

### Parser group

`lefdef` — LEF/DEF-specific `identifier` (accepts hyphens, slashes) and `line_comment` (`#`-to-EOL). The LEF `BEGINEXT … ENDEXT` vendor-extension body is captured via the grammar-level `*` raw-consume primitive (see [`rawast-format.md §4.5a-1`](rawast-format.md)), not a custom terminal parser.

## `tcl` — Tcl Tier-1 structural parser

`grammars/tcl.rawast`. Commands → words; word flavours split out; substitution segments isolated.

Uses the `subparse=` and rule-local-ignore primitives:
- Quoted `"…"` strings are captured and immediately re-parsed for `$var` / `[cmd]` substitutions (automatic via `:subparse="WORD_SEGMENTS"` in the grammar).
- Braced `{…}` blocks are captured as literals; the application re-parses them on demand when context says "this is code", e.g. the body of `if { … }` or `proc { … }`. Same engine, called recursively from host code — `Grammar.parse_string(body, start="SCRIPT")`.

**Corpus:** parses **1,440 / 1,440** OpenROAD flow-script `.tcl` files in 1.7 s.

**Parser group:** `tcl` — Tcl terminals modelled on Dodekalogue rules. `hspace`, `newline`, `comment`, `brace_group`, `quoted_string`, `bracket_sub`, `bare_word`, `expand_marker`, `var_name`, `escape`, `literal_run`. The `$arr(idx)` array-index body is captured via the grammar-level `*` primitive, not a custom `until_paren` terminal.

## Round-trip claims, summarised

| Grammar | Parse | Round-trip | Equivalence type |
|---|---|---|---|
| GDSII | 1,171 / 1,171 | 750 / 750 | byte-equivalent (binary format, full byte preservation) |
| LEF | 507 / 507 | 263 / 263 | structurally equivalent (text format, canonicalized output) |
| DEF | 436 / 436 | 435 / 435 | structurally equivalent (text format, canonicalized output) |
| Tcl | 1,440 / 1,440 | — | parse-only (round-trip not yet measured) |

**"Structurally equivalent"** means `parse → save → reparse` yields the same value tree. The output bytes may differ from the input in whitespace, comment positions, or clause ordering where the grammar makes the form irrelevant. **"Byte-equivalent"** means the output bytes match the input exactly.

## Adding a new grammar

Write a `.rawast` (DSL) or `.json` (data) file using the conventions in [`rawast-format.md`](rawast-format.md). Drop it in `grammars/`. Load it with `Grammar.load("grammars/myformat.rawast")` or address by short-name if you also stage a copy under `python/rawast/grammars/`.

Required reading for grammar authors: [`rawast-format.md`](rawast-format.md) and [`ARCHITECTURE.md`](ARCHITECTURE.md).

## See also

- [`EXAMPLES.md`](EXAMPLES.md) — worked examples per grammar
- [`rawast-format.md`](rawast-format.md) — `.rawast` grammar language reference
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — parser groups, ignore policy, subparse
- [`CLI.md`](CLI.md) — running the bundled grammars
- [`FEATURES.md`](FEATURES.md) — engine capabilities
