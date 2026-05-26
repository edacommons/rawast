# rawast

Most parsers ship as code: someone writes a grammar in BNF, compiles
it, links it into every tool that needs it. New format means a new
parser project, new compile, new tool releases. And every parser builds
a format-specific AST, even when the caller just wants one field.

rawast inverts this. The parser is one engine; the grammar is **data**
— a JSON file you load at runtime. The engine reads text and produces a
JSON-shaped value tree (arrays, dicts, scalars) — the same shape the
grammar is written in. So one engine reads any format, no recompile,
and the output is queryable without a format-specific API.

Three properties make this work: it's a structural parser driven by an
external grammar; the grammar is itself JSON-shaped data the engine can
read with itself (self-hosting); and the engine is bidirectional — the
same grammar that parses also re-emits text from a value tree. Binary
formats slot in by registering terminal parsers; GDSII is the worked
example.

The `.jast` container builds on this: grammar + parsed tree, serialised
together in a binary file. "Parse once" — every later consumer reads
the value tree directly, never re-parses text, and can still emit the
text form because the grammar travels with the data.

EDA is the first proving ground because the files are large, the
formats are many, and every tool currently reimplements its own reader.
The PoC parses real GDSII, LEF, and DEF corpora; funding is being
sought to harden it. Ships as a C++ engine with Python bindings.

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
- **Mid-parse hooks**: `on_rule_complete` + `replace_parser` for
  context-sensitive formats whose preambles configure later tokenisation
  (e.g. LEF/DEF `DIVIDERCHAR`).
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
  from a `.rawast`-described grammar).
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
  - `grammars/lef.rawast` — LEF tech-file format (LAYER /
    VIA / VIARULE / PROPERTYDEFINITIONS / SPACING + MACRO / PIN /
    PORT / OBS cell bodies). Parses 507 / 507 real LEFs across
    seven PDK / open-platform sources.
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
- **Test suite**: 235 tests (206 C++ doctest + 29 Python pytest)
  covering the engine, loader, JSON round-trip, GDSII round-trip,
  linter, callbacks, pretty-print, the `use:` directive, subparse,
  and per-rule ignore. Plus 3,132 real production files across
  GDSII / LEF / DEF / Tcl parsing 100% end-to-end (see proposal
  §2.6 for the corpus breakdown).

## What's planned

See `docs/rawast-format.md` for the language spec. Roadmap in brief:
M1 finalises the engine APIs and the grammar-load linter; M2 ships the
`.jast` container format and the `Grammar::validate()` API; M3 delivers
a CLI, Python bindings via nanobind, and a Pydantic-model generator
that emits typed Python classes from any grammar; M4 publishes the
grammar repository, ten production-quality grammars (LEF, DEF, Verilog
netlist, Liberty, SPEF, JSON, TOML, CSV, syslog, Nginx log), and
per-grammar PyPI packages.

## Quickstart (Python)

```sh
python -m venv .venv && source .venv/bin/activate
pip install -e .
rawast --help
rawast lint   grammars/gdsii.rawast
rawast parse  grammars/json.json file.json
rawast docs   grammars/gdsii.rawast   # EBNF-flavoured Markdown reference (grammar input syntax)
rawast schema grammars/gdsii.rawast   # value-tree-shape Markdown reference (dict / array / choice — what a producer tool builds before save())
```

Module use:

```python
import rawast

g = rawast.Grammar.load("grammars/json.json")
ast = g.parse_string('{"name": "alice", "items": [1, 2, 3]}')
# ast == {"name": "alice", "items": [1, 2, 3]}

text = g.save(ast)        # bytes — works for binary grammars too
issues = g.lint()         # warnings about ambiguous Choices, if any
```

Cross-format conversion in three lines:

```python
gdsii = rawast.Grammar.load("grammars/gdsii.rawast")
json_g = rawast.Grammar.load("grammars/json.json")
print(json_g.save(gdsii.parse_file("layout.gds")).decode("utf-8"))
```

Re-parse arbitrary strings through any rule in the grammar — useful
for context-dependent sub-languages (Tcl brace bodies, Liberty
attribute mini-languages):

```python
tcl = rawast.Grammar.load("grammars/tcl.rawast")
ast = tcl.parse_file("flow.tcl")
for cmd in ast.get("commands", []):
    if not cmd or cmd.get("type") != "command":
        continue
    # If the first word is "if", treat the body argument (a brace
    # word) as a nested Tcl script and re-parse it:
    words = cmd["words"]
    first = words[0]["value"]["segments"][0].get("value")
    if first == "if" and len(words) >= 3 and words[2]["type"] == "brace":
        body = tcl.parse_string(words[2]["value"], start="SCRIPT")
```

`start="RULE"` works on `parse_string`, `parse_file`, and
`parse_bytes`.

### Parser groups, `use:`, and ignore policy

Every grammar declares the parsers it needs (`use:`) and attaches the
ignore policy to whichever rule should be the default-active scope —
typically the start rule. The host loader never injects parsers or
ignores implicitly.

**`use:`** — array of parser-group names (`["std"]`, `["std", "gdsii"]`,
etc.). Each named group is registered globally at process start; `use:`
makes its parsers addressable in the grammar.

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
| `lefdef` | LEF/DEF-specific `identifier` (hyphens, slashes accepted) and `line_comment` (`#`-to-EOL) |
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
start: <FILE>
FILE ignore whitespace line_comment block_comment: sequence dict { ... }
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
  rawast/              Python package
  tests/               pytest suite
```

## Documentation

- [`docs/rawast-format.md`](docs/rawast-format.md) — the `.rawast`
  grammar language specification.

## License

MIT — see [LICENSE](LICENSE).

## Author

Serge Rabyking · [LinkedIn](https://linkedin.com/in/serge-rabyking-b556ab89)
