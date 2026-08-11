# rawast

[![CI](https://github.com/edacommons/rawast/actions/workflows/ci.yml/badge.svg)](https://github.com/edacommons/rawast/actions/workflows/ci.yml)
[![PyPI](https://img.shields.io/pypi/v/rawast)](https://pypi.org/project/rawast/)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Website](https://img.shields.io/badge/site-rawast.org-3e63dd)](https://rawast.org)

Most parsers do too much. They build a semantic model of the file when all you need is its **structure** — scopes, lists, fields. The meaning belongs to the application reading the parsed data, not to the parser itself. But the lex+yacc tradition forces both: tokenize first, then reconstruct everything with a state machine. Covering a full format that way is enormous work — and almost never finished.

**rawast formalizes the structure-first approach as a universal bidirectional grammar-driven engine for structured text and binary formats.** Every EDA tool today reimplements its own readers for LEF, DEF, GDSII, Liberty, and every other format the field uses — every one re-parsing the same files. rawast inverts that: **one engine, grammars as data files, and a binary container that distributes parsed data so downstream consumers never re-parse text at all.** Ships as a C++17 library with Python bindings.

The parser is one engine; the grammar is **data** — a JSON / `.rawast` file you load at runtime. The engine reads text or bytes and produces a JSON-shaped value tree (arrays, dicts, scalars). One engine reads any format, no recompile. The output is queryable without a format-specific API.

Three properties make this work: it's a structural parser driven by an external grammar; the grammar is itself JSON-shaped data the engine can read with itself (self-hosting); and the engine is bidirectional — the same grammar that parses also re-emits text from a value tree. Binary formats slot in by registering terminal parsers; **GDSII** — the standard binary format for IC layout — is the worked example.

The planned `.jast` container builds on this: grammar + parsed tree, serialised together in a binary file. "Parse once" — every later consumer reads the value tree directly, never re-parses text, and can still emit the text form because the grammar travels with the data. See [`docs/ROADMAP.md`](docs/ROADMAP.md).

EDA is the first proving ground because the files are large, the formats are many, and every tool currently reimplements its own reader and writer. The PoC parses 100% of a 3,132-file production corpus across four formats (GDSII / LEF / DEF / Tcl). [`docs/ROADMAP.md`](docs/ROADMAP.md) is the path to 1.0.

## Install

```sh
python -m venv .venv && source .venv/bin/activate
pip install rawast
```

Compiles the C++ engine from source (no pre-built wheels yet) — needs **C++17** (GCC 7+, Clang 5+, Apple Clang 9+, MSVC 2017+) and **CMake** 3.20+ on your `PATH`. Compile takes ~15–20 seconds on a modern laptop. Zero runtime Python dependencies.

For development against the repo, see [`docs/BUILD.md`](docs/BUILD.md).

## First 60 seconds

```python
import rawast

g = rawast.Grammar("json")    # bundled grammar by short name
ast = g.parse_string('{"name": "alice", "items": [1, 2, 3]}')
# ast == {"name": "alice", "items": [1, 2, 3]}

text = g.save(ast)            # bytes — works for binary grammars too
issues = g.lint()             # warnings about ambiguous Choices, if any
```

Bundled grammars: `Grammar("json")`, `Grammar("rawast")`, `Grammar("gdsii")`, `Grammar("lefdef")`, `Grammar("tcl")`. Load your own with `Grammar.load("path/to/my_format.rawast")`.

Cross-format conversion in three lines:

```python
gdsii  = rawast.Grammar("gdsii")
json_g = rawast.Grammar("json")
print(json_g.save(gdsii.parse_file("layout.gds")).decode("utf-8"))
```

`parse_file` / `parse_string` / `parse_bytes` cover the common cases. If you already hold a stream — say, the expanded output of a preprocessor, or one Stream you want to consume through several grammars in sequence — use the canonical `parse_stream` entry instead:

```python
stream = rawast.Stream.from_file("layout.gds")
ast    = gdsii.parse_stream(stream)
```

`rawast.Stream` is the canonical parser-input type as of v0.1.9 — `parse_string` / `parse_file` / `parse_bytes` are sugar over `Stream.from_string` / `Stream.from_file`. You can construct your own Stream and pass it to any grammar.

### Preprocessing pipeline (SystemVerilog and similar)

The `Preprocessor` exposes the same pipeline at three independent joints:

```python
g    = rawast.Grammar("systemverilog")   # one grammar: SV + preprocessor
pp   = rawast.Preprocessor(g)            # enters at the PP_FILE rule

src  = open("design.sv").read()
ast  = pp.parse(src)                     # Mode 1: directives as AST, no expansion
stream = pp.preprocess(ast, src)         # Mode 2: expand macros, returns Stream
top  = g.parse_stream(stream)            # Mode 3: host parse on the expanded bytes
```

Useful when you want to inspect macro / `\`include` / `\`ifdef` structure (Mode 1) without paying expansion cost, or when one stage of a build pipeline expands and another consumes. `pp.process(src)` still works as the one-call "give me the expanded bytes" shortcut.

CLI:

```sh
rawast --help
rawast parse    grammars/json.json file.json
rawast pydantic grammars/lefdef.rawast > models.py   # typed Pydantic v2 models
rawast pycode   grammars/lefdef.rawast file.lef \
                --start LEF --models-module models   # Python source that reconstructs the model
```

Full reference: [`docs/CLI.md`](docs/CLI.md).

## Documentation

| | What |
|---|---|
| [`docs/FEATURES.md`](docs/FEATURES.md) | All engine capabilities — parsing, save, profiling, Pydantic + pycode, perf wins |
| [`docs/CLI.md`](docs/CLI.md) | Every CLI command, every flag, with examples |
| [`docs/EXAMPLES.md`](docs/EXAMPLES.md) | Worked examples per capability — parse / save, cross-format, Pydantic + pycode, Tcl recursion, GDSII binary, linting, profiling |
| [`docs/AGENTS.md`](docs/AGENTS.md) | Using rawast with LLM tools and agents — why structured AST beats text-pattern matching, what an agent should read to author a grammar, prompt structure |
| [`docs/GRAMMARS.md`](docs/GRAMMARS.md) | Shipped grammars (GDSII / LEF / DEF / Tcl / JSON / rawast meta) with corpus numbers |
| [`docs/BUILD.md`](docs/BUILD.md) | Building from source — Python, C++ library, sdist |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Engine internals — parser groups, `use:`, ignore policy, subparse, the bidirectional walk |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Path to 1.0 — M1–M4, funding context |
| [`docs/rawast-format.md`](docs/rawast-format.md) | The `.rawast` grammar language specification |
| [`examples/`](examples/) | Runnable scripts |
| [`SECURITY.md`](SECURITY.md) | Vulnerability-reporting policy |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | How to build, test, submit changes |

## History

rawast is the C++ rewrite of an earlier Python prototype (2023–2025) that validated the data-driven grammar approach, the catcher-based value-tree mechanism, and the bidirectional walk. The current implementation is the productionisation of those ideas as a maintained C++17 codebase; most of the commit history here reflects the rewrite phase. Design decisions and the architecture they came from are documented in `docs/` and in the prototype's history.

## Repository layout

```
include/rawast/      public C++ API headers
src/                 engine implementation
grammars/            community-maintained grammars (.rawast and .json)
docs/                language, feature, CLI, grammar, build, architecture, roadmap docs
tests/               doctest-based C++ test suite
python/              Python binding + CLI (nanobind extension module)
  src/native.cc        binding implementation
  rawast/              Python package (CLI in cli.py; docs/schema generators in docs.py / schema.py)
  tests/               pytest suite
examples/            small worked examples (parse → modify → save, etc.)
```

## Funding

The work outlined in [`docs/ROADMAP.md`](docs/ROADMAP.md) is the basis of the NLnet NGI0 Commons funding application. Sponsorship via GitHub Sponsors at https://github.com/sponsors/lanserge is the most direct way to help.

Beyond the roadmap milestones, **adopt a format**: a sponsorship puts one
more format into the engine as a grammar — Liberty ([#15](https://github.com/edacommons/rawast/issues/15)),
SPEF ([#16](https://github.com/edacommons/rawast/issues/16)),
SDC ([#17](https://github.com/edacommons/rawast/issues/17)),
OASIS ([#18](https://github.com/edacommons/rawast/issues/18)),
SPICE ([#19](https://github.com/edacommons/rawast/issues/19)),
EDIF ([#20](https://github.com/edacommons/rawast/issues/20)),
Verilog-2001 ([#5](https://github.com/edacommons/rawast/issues/5)). "Done" is
objective: the grammar lands as data, a real-file corpus parses 100%, and
re-emission round-trips byte-exact — and an adopting organisation's own
corpus can prove it privately: your files prove it, your name ships on it,
your files stay yours.

## License

MIT — see [LICENSE](LICENSE).

## Author

Serge Rabyking · [serge.rabyking.com](https://serge.rabyking.com)
