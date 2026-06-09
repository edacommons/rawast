# rawast

Most parsers do too much. They build a semantic model of the file when all you need is its **structure** — scopes, lists, fields. The meaning belongs to the application reading the parsed data, not to the parser itself. But the lex+yacc tradition forces both: tokenize first, then reconstruct everything with a state machine. Covering a full format that way is enormous work — and almost never finished.

**rawast formalizes the structure-first approach as a universal bidirectional grammar-driven engine for structured text and binary formats.** Every EDA tool today reimplements its own readers for LEF, DEF, GDSII, Liberty, and every other format the field uses — every one re-parsing the same files. rawast inverts that: **one engine, grammars as data files, and a binary container that distributes parsed data so downstream consumers never re-parse text at all.** Ships as a C++17 library with Python bindings.

The parser is one engine; the grammar is **data** — a JSON / `.rawast` file you load at runtime. The engine reads text or bytes and produces a JSON-shaped value tree (arrays, dicts, scalars). One engine reads any format, no recompile. The output is queryable without a format-specific API.

Three properties make this work: it's a structural parser driven by an external grammar; the grammar is itself JSON-shaped data the engine can read with itself (self-hosting); and the engine is bidirectional — the same grammar that parses also re-emits text from a value tree. Binary formats slot in by registering terminal parsers; **GDSII** — the standard binary format for IC layout — is the worked example.

The planned `.jast` container builds on this: grammar + parsed tree, serialised together in a binary file. "Parse once" — every later consumer reads the value tree directly, never re-parses text, and can still emit the text form because the grammar travels with the data. See [`docs/ROADMAP.md`](docs/ROADMAP.md).

EDA is the first proving ground because the files are large, the formats are many, and every tool currently reimplements its own reader and writer. The PoC parses 100% of a 3,132-file production corpus across four formats (GDSII / LEF / DEF / Tcl); funding is being sought to turn the PoC into shippable infrastructure.

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

## License

MIT — see [LICENSE](LICENSE).

## Author

Serge Rabyking · [LinkedIn](https://linkedin.com/in/serge-rabyking-b556ab89)
