# rawast

**A universal bidirectional grammar-driven engine for structured text
and binary formats.** Every EDA tool, today, reimplements its own
readers for LEF, DEF, GDSII, Liberty, and every other format the field
uses — and every one of them re-parses the same files. rawast inverts
that: **one engine, grammars as data files, and a binary container
that distributes parsed data so downstream consumers never re-parse
text at all.** Ships as a C++20 library with Python bindings.

The parser is one engine; the grammar is **data** — a JSON / `.rawast`
file you load at runtime. The engine reads text or bytes and produces
a JSON-shaped value tree (arrays, dicts, scalars). One engine reads
any format, no recompile. The output is queryable without a
format-specific API.

Three properties make this work: it's a structural parser driven by an
external grammar; the grammar is itself JSON-shaped data the engine
can read with itself (self-hosting); and the engine is bidirectional —
the same grammar that parses also re-emits text from a value tree.
Binary formats slot in by registering terminal parsers; **GDSII** —
the standard binary format for IC layout — is the worked example.

The planned `.jast` container builds on this: grammar + parsed tree,
serialised together in a binary file. "Parse once" — every later
consumer reads the value tree directly, never re-parses text, and can
still emit the text form because the grammar travels with the data.
The format is designed and specified; the engine that will read and
write it is the bidirectional walk that already handles source-text
parse and save today. See *What's planned* below.

EDA is the first proving ground because the files are large, the
formats are many, and every tool currently reimplements its own reader
and writer. The PoC parses 100% of a 3,132-file production corpus
across four formats (GDSII / LEF / DEF / Tcl); funding is being sought
to turn the PoC into shippable infrastructure.

## History

rawast is the C++ rewrite of an earlier Python prototype (2023–2025)
that validated the data-driven grammar approach, the
catcher-based value-tree mechanism, and the bidirectional walk. The
current implementation is the productionisation of those ideas as a
maintained C++20 codebase; most of the commit history here reflects the
rewrite phase. Design decisions and the architecture they came from
are documented in `docs/` and in the prototype's history.

## What works today

- **Predictive PEG parsing** with per-`Choice` opt-in bounded
  backtracking; structural linter at grammar-load time flagging LL(1)
  violations.
- **Bidirectional walk**: parse and save share one grammar definition.
  The save direction uses a stack-navigation walk with key-based Choice
  dispatch, wrapped-substructure descent, and catch-all alternatives —
  enough machinery that the `.rawast` meta-grammar can save its own
  parsed grammars back as canonical `.rawast` text (self-host save).
  Pretty-print attributes (`indent`, `tab`, `space`, `newline`,
  `tail="..."`) plus a `pretty=true/false` toggle let one grammar
  cover both compact and human-readable output.
- **Value model**: typed AST (`null`/`bool`/`int`/`uint`/`real`/`string`/
  `array`/`dict`) with primitive interning and back-references for
  post-parse value search.
- **Subparse + rule-local ignore overrides** — two engine primitives
  for composing languages-within-languages in a single grammar file.
  `:subparse="<RULE>"` on a Parse-terminal item re-invokes the parse
  loop on the captured string with a different rule as start; same
  grammar, fresh ignore-stack, fully recursive. `RULE ignore X Y: …`
  attaches a rule-local ignore override; the parse driver pushes the
  override on rule entry and pops on exit; rules without an override
  inherit the caller's active ignore. Together they make multi-context
  grammars (script + embedded expression + token-internals split-out)
  a single-file artefact rather than three loosely coupled grammars.
  Demonstrated by the Tcl grammar (below).
- **`.rawast` grammar language** — concise hand-written DSL for
  grammars, fully self-hosted (the `.rawast` parser is itself loaded
  from a `.rawast`-described grammar). Recent additions: list-
  append binding (`:name[]=@`) so a single grammar can capture
  multi-instance clauses losslessly without giving up the catcher
  convenience for single-instance ones (see `docs/rawast-format.md
  §4.5a`).
- **Pydantic v2 model generator** — `rawast pydantic <grammar>`
  emits a ready-to-import Python module whose classes mirror the
  grammar's parse/save dict shape exactly. Round-trip contract:
  `Class.model_validate(g.parse_file(p)).model_dump(...)` equals
  the parsed dict. The model rejects any field the grammar can't
  write back (`ConfigDict(extra="forbid")`), so the user-facing
  API is **construct-and-save**: build a LEF in Python, dump to
  dict, hand to `g.save(...)`. Discriminated unions, list-append
  bindings, nested sub-dicts, and forward references all flow
  through to typed Python fields.
- **`use:` directive** — grammars declare which terminal-parser groups
  they need; the loader resolves names against a static registry of
  built-in groups.
- **Working grammars in the repo**:
  - `grammars/json.json` — full JSON with array/dict containers.
  - `grammars/rawast.json` / `grammars/rawast.rawast` — the
    `.rawast` meta-grammar (self-host).
  - `grammars/gdsii.rawast` — the GDSII binary IC-layout format. All
    47 record types, the seven element kinds, full structural schema.
    Parses 1,171 / 1,171 real production GDSII files from open-PDK
    chip flows (Sky130, GF180MCU, IHP130, asap7, gf180, ihp-sg13g2).
    Synthetic round-trip tests cover save back to byte-identical
    output (including 2,048-byte alignment padding).
  - `grammars/lef.rawast` — LEF base-spec (5.8) coverage, less
    LEF58_* (deferred to a consumer-supplied sub-grammar). LAYER
    blocks expose per-TYPE typed fields (`layer_type`, `direction`,
    `pitch`, `width`, …); VIA blocks model both the geometry and
    VIARULE-based forms; VIARULE blocks model the LAYER-pair and
    GENERATE forms with typed sub-clauses (`enclosure`, `rect`,
    `spacing`); PIN/MACRO bodies cover every spec sub-statement
    including DENSITY, MUSTJOIN, EEQ, FIXEDMASK, and the full
    ANTENNA*-family list capture. Parses 507 / 507 real LEFs across
    seven PDK / open-platform sources; round-trip-tested against
    the Sky130 tech LEF, the OpenRAM SRAM, the `top_xres4v2` IO
    pad, a multi-ANTENNA HD cell, and a synthetic full-spec fixture
    that exercises every documented clause.
  - `grammars/def.rawast` — DEF placement-and-route file format
    (PINS / BLOCKAGES / VIAS / COMPONENTS / NONDEFAULTRULES /
    SPECIALNETS / NETS / FILLS / GCELLGRID / PROPERTYDEFINITIONS).
    Parses 14 / 14 production DEFs including four 4.1–4.8M-line
    placement-and-route outputs.
  - `grammars/tcl.rawast` — Tcl Tier-1 structural parser
    (commands → words; word flavours split out; substitution
    segments isolated). Uses the new subparse + rule-local-ignore
    primitives. Parses 1,440 / 1,440 OpenROAD flow-script `.tcl`
    files in 1.7 s.
- **Bidirectional grammar conversion**: the `.rawast` meta-grammar
  loads grammars as data (`Grammar.from_dict`, `meta.parse_file`) and
  writes them back via the same save engine. Parse a `.rawast` file,
  modify the AST, emit it back as canonical `.rawast` text — round-
  trips structurally identical.
- **Test suite**: 252 tests (208 C++ doctest + 44 Python pytest)
  covering the engine, loader, JSON round-trip, GDSII round-trip,
  linter, pretty-print, the `use:` directive, subparse, per-rule
  ignore, list-append binding, the data-shape schema generator,
  and the Pydantic-model generator (round-trip on a synthetic full-
  LEF-spec fixture plus four real Sky130 PDK files: the HD tech
  LEF, an OpenRAM SRAM, the `top_xres4v2` IO pad, and a multi-
  ANTENNA HD cell). Plus 3,132 real production files across GDSII
  / LEF / DEF / Tcl parsing 100% end-to-end.

## What's planned

See `docs/rawast-format.md` for the language spec. The roadmap to 1.0:

- **Typed Python developer surface.** A structural-validation API for
  host-constructed value trees with path-aware errors (separate from
  the Pydantic-model generator, which already ships — above); the
  data-shape reference generator productionised. Sub-parse-aware
  error reporting and an expanded grammar linter.
- **The `.jast` binary container.** A self-describing binary file
  carrying manifest + grammar + value tree with one internal value
  pool, plus primitive value interning wired through the in-memory
  parse path. Value-search API; pretty-print save mode. Downstream
  consumers mmap the structured tree from disk — no re-parsing.
- **Cross-platform distribution.** CMake + GitHub Actions builds on
  Linux / macOS / Windows; PyPI wheels; `find_package(rawast)` for
  C++ consumers; CLI rounded out with `.jast` compile / decompile,
  validate, pretty, diff; first-cut user documentation.
- **Community grammar repository at 1.0.** Polished, spec-audited
  grammars for **LEF, DEF, Verilog netlist, SDC, SPICE netlist, JSON,
  TOML** (Liberty, SPEF, and SDF as stretch). Each shipped grammar
  with an auto-generated EBNF reference page, an auto-generated data-
  shape reference page, and a structural test corpus. Outreach to
  open-source EDA project maintainers; a `.jast` PDK proof-of-concept
  with measured download-size and cold-ingestion comparisons.

## Quickstart (Python)

`pip install` compiles the C++ engine via scikit-build-core under the
hood — you need a **C++20 compiler** (GCC 11+, Clang 14+, or Apple
Clang 14+) and **CMake** (3.20+) available on your `PATH` first. No
separate `cmake` step is required for Python use.

All Python build dependencies (`scikit-build-core`, `nanobind`) and
C++ dependencies (`tl::expected`; `doctest` only for the C++ test
build) are pulled automatically by pip and CMake. rawast itself has
**no runtime Python dependencies** — `import rawast` works cleanly
with zero extra packages installed.

```sh
python -m venv .venv && source .venv/bin/activate
pip install -e .
rawast --help
rawast lint     grammars/gdsii.rawast
rawast parse    grammars/json.json file.json
rawast docs     grammars/gdsii.rawast      # EBNF-flavoured Markdown reference (grammar input syntax)
rawast schema   grammars/gdsii.rawast      # value-tree-shape Markdown reference (dict / array / choice — what a producer tool builds before save())
rawast pydantic grammars/lef.rawast        # generate Pydantic v2 models matching the grammar's parse/save dict shape exactly
```

The generated Pydantic module is round-trip-faithful: a parsed
value validates as a model, `model_dump()` reproduces the input
dict, and the model rejects any field the grammar can't save back
(`ConfigDict(extra="forbid")`). Discriminated unions, list-append
bindings (`:name[]=@`), and nested sub-dicts all flow through to
typed Python fields — see `docs/rawast-format.md §4.5a` and the
construction-toolkit memory for the contract.

Module use:

```python
import rawast

g = rawast.Grammar("json")    # bundled grammar by short name
ast = g.parse_string('{"name": "alice", "items": [1, 2, 3]}')
# ast == {"name": "alice", "items": [1, 2, 3]}

text = g.save(ast)            # bytes — works for binary grammars too
issues = g.lint()             # warnings about ambiguous Choices, if any
```

The bundled grammars are addressable by short name: `Grammar("json")`,
`Grammar("rawast")`, `Grammar("gdsii")`, `Grammar("lef")`,
`Grammar("def")`, `Grammar("tcl")`. To load your own grammar from
disk, use `Grammar.load`:

```python
g = rawast.Grammar.load("path/to/my_format.rawast")
ast = g.parse_file("input.txt")
```

Cross-format conversion in three lines:

```python
gdsii  = rawast.Grammar("gdsii")
json_g = rawast.Grammar("json")
print(json_g.save(gdsii.parse_file("layout.gds")).decode("utf-8"))
```

Re-parse arbitrary strings through any rule in the grammar — useful
for context-dependent sub-languages (Tcl brace bodies, Liberty
attribute mini-languages):

```python
tcl = rawast.Grammar("tcl")
ast = tcl.parse_file("flow.tcl")
for cmd in ast.get("commands", []):
    if not cmd or cmd.get("type") != "command":
        continue
    # If the first word is "if", treat the body argument (a brace
    # word) as a nested Tcl script and re-parse it:
    words = cmd["words"]
    first = words[0]["value"]["segments"][0].get("value")
    if first == "if" and len(words) >= 3 and words[2]["type"] == "brace":
        # Re-enter the parser at the SCRIPT rule on the brace body.
        # SCRIPT happens to be the tcl grammar's default start rule,
        # so passing `start="SCRIPT"` here is explicit but redundant;
        # pass a different rule name (e.g. start="WORD_SEGMENTS") to
        # re-enter at a sub-language's entry point instead.
        body = tcl.parse_string(words[2]["value"], start="SCRIPT")
```

`start="RULE"` works on `parse_string`, `parse_file`, and
`parse_bytes`.

### Parser groups, `use:`, and ignore policy

Every grammar declares the parsers it needs (`use:`) and attaches the
ignore policy to whichever rule should be the default-active scope —
typically the start rule. The host loader never injects parsers or
ignores implicitly.

**`use:`** — list of parser-group names. In `.rawast` form,
comma-separated bare identifiers (`use: std`, `use: std, gdsii`); in
JSON form, a JSON array (`"use": ["std", "gdsii"]`). Each named group
is registered globally at process start; `use:` makes its parsers
addressable in the grammar.

**`RULE ignore PARSER1 PARSER2 …: <body>`** — a per-rule attribute
declaring the ignore list for that rule's sub-tree. The parse driver
maintains an ignore-stack; on rule entry an explicit override is
pushed and on exit popped. Rules without the attribute inherit the
caller's active ignore. Empty list (`RULE ignore: …`) means
"ignore nothing" — useful for token-internal contexts where
whitespace is part of the data.

Each parser is addressable under **two names**: bare (`int`) and
qualified (`std.int`). Bare works when unambiguous; qualified is
self-documenting and disambiguates across groups that share local
names. Both forms resolve to the same parser.

Shipped groups:

| Group | Parsers |
|---|---|
| `std` | `int`, `uint`, `float`, `identifier`, `qualified_identifier`, `string`, `whitespace`, `line_comment`, `block_comment` |
| `gdsii` | All 47 GDSII record-type parsers (`header`, `bgnlib`, …, `endmasks`) — bare or `gdsii.header` form |
| `lefdef` | LEF/DEF-specific `identifier` (hyphens, slashes accepted), `line_comment` (`#`-to-EOL), and `until_endext` (raw text consumed up to the next `ENDEXT` keyword — captures the inner body of a LEF `BEGINEXT … ENDEXT` vendor-extension block opaquely) |
| `tcl` | Tcl terminals modelled on Dodekalogue rules — `hspace`, `newline`, `comment`, `brace_group`, `quoted_string`, `bracket_sub`, `bare_word`, `expand_marker`, `var_name`, `until_paren`, `escape`, `literal_run` |

Shipped grammars:

```
# grammars/json.json — strict JSON (RFC 8259)
{ "start": "VALUE",
  "use":    ["std"],
  "VALUE":  { "type": "choice",
              "rule_ignore": ["whitespace"],   // attached to start rule
              "items": [ ... ] },
  ... }
```

```
# grammars/rawast.rawast — JSONC meta-grammar (self-host)
use: std
start: <GRAMMAR>
GRAMMAR ignore whitespace line_comment block_comment: sequence dict { ... }
```

```
# grammars/gdsii.rawast — binary, no ignores
use: gdsii
start: <LIBRARY>
LIBRARY: sequence dict { ... }
```

```
# grammars/tcl.rawast — multi-context grammar with subparse +
# rule-local ignore overrides
use: std, tcl
start: <SCRIPT>
SCRIPT ignore tcl.hspace: sequence dict { ... }
WORD_SEGMENTS ignore: sequence dict { ... }    // override to ignore nothing
```

The in-memory `make_json_grammar()` (C++) is **JSONC by construction**
— it applies the `std` group internally and adds whitespace + comments
to its ignore list. This is the bootstrap grammar used to read JSON-
form grammar files (which typically carry inline `//` and `/* */` docs).

## Build (C++ library and tests)

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires a C++20-capable compiler:

- GCC 11 or newer
- Clang 14 or newer
- Apple Clang 14 or newer

Dependencies (fetched automatically by CMake):

- [tl::expected](https://github.com/TartanLlama/expected) — error model, header-only.
- [doctest](https://github.com/doctest/doctest) — test framework, single-header, test-only.
- [nanobind](https://github.com/wjakob/nanobind) — Python binding generator (only when building the Python module).

## Repository layout

```
include/rawast/      public C++ API headers
src/                 engine implementation
grammars/            community-maintained grammars (.rawast and .json)
docs/                language and architecture documentation
tests/               doctest-based C++ test suite
python/              Python binding + CLI (nanobind extension module)
  src/native.cc        binding implementation
  rawast/              Python package (CLI in cli.py; docs/schema
                       generators in docs.py / schema.py)
  tests/               pytest suite
examples/            small worked examples (parse → modify → save, etc.)
```

## Documentation

- [`docs/rawast-format.md`](docs/rawast-format.md) — the `.rawast`
  grammar language specification.
- [`examples/`](examples/) — small worked examples.
- [`SECURITY.md`](SECURITY.md) — vulnerability-reporting policy.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — how to build, test, and
  submit changes.

## License

MIT — see [LICENSE](LICENSE).

## Author

Serge Rabyking · [LinkedIn](https://linkedin.com/in/serge-rabyking-b556ab89)
