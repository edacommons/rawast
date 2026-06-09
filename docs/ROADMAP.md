# Roadmap

The path from where we are today to 1.0. Items here are scoped at the milestone level; the actual implementation work is broken down on GitHub issues and the project board.

See [`FEATURES.md`](FEATURES.md) for what already ships. See [`GRAMMARS.md`](GRAMMARS.md) for current grammar coverage and corpus numbers.

## M1 — Typed Python developer surface

The Pydantic v2 generator and `pycode` Python source emitter already ship today (see FEATURES.md). Remaining for M1:

- **Structural-validation API** for host-constructed value trees with path-aware errors. Separate from the Pydantic-model generator: lets a host build a partial tree and ask "is this shape valid for this grammar at rule X?". The current path is "construct via Pydantic, hand the dict to `g.save`, handle save-time errors"; the validation API gives faster, more localized feedback.
- **Data-shape reference generator** productionised. `rawast schema` exists and works; gaps remaining are around discriminated-union rendering, nested-list-of-union edge cases, and stable cross-rule anchors.
- **Sub-parse-aware error reporting.** When a sub-parse (`:subparse="X"`) fails, the engine currently reports the inner parser's error against the inner string. Plumb context through so the caller sees "in the body of the `if` at outer.tcl:42, expected …".
- **Expanded grammar linter.** Today the linter catches LL(1) violations, the nested-Choice-type-emit anti-pattern, and `*` misuse. Add: unreachable rules; binding-shape mismatches between sibling alts; sub-parse target reachability.

## M2 — The `.jast` binary container

The format is designed and specified. The engine that will read and write it is the bidirectional walk that already handles source-text parse and save today. Remaining work:

- **Self-describing binary file** carrying manifest + grammar + value tree.
- **One internal value pool** per file, plus primitive value interning wired through the in-memory parse path. The in-memory pool already exists; M2 wires it through to the `.jast` writer/reader.
- **Value-search API** — given a parsed tree (or a mapped `.jast`), find all `(rule, field)` matches matching a predicate. Pretty-print save mode that prints those matches as paths.
- **mmap-from-disk read path.** Downstream consumers open a `.jast`, mmap the structured tree, and walk it directly. No re-parsing of text. This is the "parse once" payoff for PDK distribution: every tool that links librawast reads the same canonical tree.

## M3 — Cross-platform distribution

The sdist + GitHub Actions release workflow already work (see [`BUILD.md`](BUILD.md)). Remaining:

- **`cibuildwheel`** for pre-built wheels on Linux / macOS / Windows × x86_64 / arm64. With `nanobind`'s `abi3` stable-ABI wheels, the matrix collapses from "per Python minor version" to "one wheel per platform" — roughly 6 wheels per release instead of 30.
- **`find_package(rawast)`** for C++ consumers. CMake install rules exist; need a polished export target + a smoke-test consumer project to verify it.
- **CLI rounded out** with `.jast` compile / decompile, validate, pretty, diff.
- **First-cut user documentation** — the README split (this file is part of it) is a step; a user guide proper, with worked examples per grammar, is M3 scope.

## M4 — Community grammar repository at 1.0

The 1.0 release ships polished, spec-audited grammars for the practical EDA stack. Six grammars ship today (see [`GRAMMARS.md`](GRAMMARS.md)). M4 adds:

- **Verilog netlist** — structural netlist subset (post-synthesis, no behavioural / generate constructs). Pairs with LEF + DEF for the full place-and-route export.
- **SDC** — Synopsys Design Constraints. Tcl-based; reuses the Tcl grammar's word/substitution structure with a typed-command layer on top.
- **SPICE netlist** — Berkeley SPICE3 + common vendor extensions (HSPICE, ngspice). Subcircuit / model / parameter coverage.
- **TOML** — for project metadata, pyproject-style config, etc. Trivial to add once Liberty unblocks.
- **Liberty (`.lib`) — stretch goal.** Timing tables, leakage, multi-corner. Liberty is large and ambiguous in places; grammar quality is more important than coverage breadth here.
- **SPEF — stretch goal.** Standard Parasitic Exchange Format. Less ambiguous than Liberty but the volume per file makes parse-performance work important.
- **SDF — stretch goal.** Standard Delay Format.

Each shipped grammar comes with:
- An auto-generated EBNF reference page (output of `rawast docs`).
- An auto-generated data-shape reference page (output of `rawast schema`).
- A structural test corpus + round-trip claim with concrete numbers.

Plus:
- **Outreach to open-source EDA project maintainers** — Yosys, OpenROAD, KLayout, Coriolis, others. Goal: pilot integrations replacing per-tool parsers with rawast-mediated reads.
- **A `.jast` PDK proof-of-concept** — pick one open PDK (Sky130 or IHP130), distribute the LEF/Liberty/SPEF stack as `.jast`, measure download size + cold-ingestion vs the current text-based distribution.

## Funding

The work on M1–M4 is the basis of the NLnet NGI0 Commons funding application. Status (as of mid-2026): submitted; funding decisions ramping. See `PROPOSAL_v2.md` (local-only) for the detailed proposal.

Sponsorship via GitHub Sponsors at https://github.com/sponsors/lanserge is the most direct way to help right now — the M1–M4 timeline scales with how much of Serge's time can be spent on this vs other commitments.

## See also

- [`FEATURES.md`](FEATURES.md) — what already ships
- [`GRAMMARS.md`](GRAMMARS.md) — current grammar coverage
- [`BUILD.md`](BUILD.md) — current distribution path (sdist)
- [`CLI.md`](CLI.md) — current command surface
