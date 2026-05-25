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
- **`.rawast` grammar language** — concise hand-written DSL for grammars,
  fully self-hosted (the `.rawast` parser is itself loaded from a
  `.rawast`-described grammar).
- **`use:` directive** — grammars declare which terminal-parser groups
  they need; the loader resolves names against a static registry of
  built-in groups.
- **Working grammars in the repo**:
  - `grammars/json.json` — full JSON with array/dict containers.
  - `grammars/rawast.json` — the `.rawast` meta-grammar (self-host).
  - `grammars/gdsii.rawast` — the GDSII binary IC-layout format. All
    47 record types, the seven element kinds, full structural schema.
    Real production GDSII files from open-PDK chip flows (GF180MCU and
    IHP130) parse cleanly; synthetic round-trip tests cover save back
    to byte-identical output.
- **Bidirectional grammar conversion**: the `.rawast` meta-grammar
  loads grammars as data (`Grammar.from_dict`, `meta.parse_file`) and
  writes them back via the same save engine. Parse a `.rawast` file,
  modify the AST, emit it back as canonical `.rawast` text — round-
  trips structurally identical.
- **Test suite**: 200+ tests covering the engine, loader, JSON
  round-trip, GDSII round-trip, linter, callbacks, pretty-print, and
  the `use:` directive.

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
rawast lint  grammars/gdsii.rawast
rawast parse grammars/json.json file.json
rawast docs  grammars/gdsii.rawast   # EBNF-flavoured Markdown reference
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

### Parser groups, `use:`, and ignore declarations

Every grammar declares the parsers it needs and which of them to skip
between tokens. The host loader never injects parsers or ignores
implicitly. Two top-level fields drive this:

- **`"use"`**: an array of parser-group names (e.g. `["std"]`,
  `["std", "gdsii"]`). Each named group is registered globally at
  process start; `use:` makes its parsers addressable in the grammar.
- **`"ignore"`**: an array of parser names whose matches are silently
  consumed between tokens (whitespace, comments, …). Names can be
  bare (`"whitespace"`) or qualified (`"std.whitespace"`).

Each parser is addressable under **two names**: bare (`int`) and
qualified (`std.int`). Bare works when unambiguous; qualified is
self-documenting and disambiguates across groups that share local
names. Both forms resolve to the same parser.

Shipped groups:

| Group | Parsers |
|---|---|
| `std` | `int`, `float`, `identifier`, `string`, `whitespace`, `line_comment`, `block_comment` |
| `gdsii` | All 47 GDSII record-type parsers (`header`, `bgnlib`, …, `endmasks`) — bare names; `gdsii.header` form also resolves |

Shipped grammars:

```json
// grammars/json.json — strict JSON (RFC 8259)
{ "start": "VALUE",
  "use":    ["std"],
  "ignore": ["whitespace"],
  ... }
```

```json
// grammars/rawast.json — JSONC meta-grammar
{ "start": "FILE",
  "use":    ["std"],
  "ignore": ["whitespace", "line_comment", "block_comment"],
  ... }
```

```
# grammars/gdsii.rawast — binary, no ignores
use: gdsii
start: <LIBRARY>
...
```

The in-memory `make_json_grammar()` (C++) is **JSONC by construction**
— it applies the `std` group internally and adds whitespace + comments
to its ignore list. This is the bootstrap grammar used to read JSON-
form grammar files (which typically carry inline `//` and `/* */` docs).

In Python, `Grammar.load(path)` reads `use:` and `ignore:` from the
file and applies them. Nothing is added implicitly — what the grammar
declares is what it accepts.

> The `.rawast` text format does not yet have syntactic `ignore:` or
> structured `use:` array directives parallel to JSON form (only
> `use: <group>` single-token). Text grammars authored in `.rawast`
> that need ignore declarations should use JSON form for now;
> bringing parity to `.rawast` is M1 scope.

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
