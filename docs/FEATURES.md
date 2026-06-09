# Features

What rawast can do today. Most features were validated against a 3,132-file production corpus across four formats; see [`GRAMMARS.md`](GRAMMARS.md) for the per-grammar corpus numbers.

## Engine

- **Predictive PEG parsing** with per-`Choice` opt-in bounded backtracking. The default is first-token commit; opt in to `backtrack: true` on a Choice that needs it.
- **Structural linter at grammar-load time** flagging LL(1) violations and a wildcard-rule-with-nested-Choice-type-emit anti-pattern that breaks save dispatch. Invoke via `rawast lint <grammar>` or `Grammar.lint()`. The lint catches real grammar-design bugs before they fail mysteriously at save time.
- **Bidirectional walk** — parse and save share one grammar definition. The save direction uses a stack-navigation walk with key-based Choice dispatch, wrapped-substructure descent, and catch-all alternatives. The `.rawast` meta-grammar can save its own parsed grammars back as canonical `.rawast` text (self-host save).
- **Pretty-print attributes** — `indent`, `tab`, `space`, `newline`, `tail="…"` plus a `pretty=true/false` toggle let one grammar cover both compact and human-readable output.
- **Subparse** — `:subparse="<RULE>"` on a Parse-terminal item re-invokes the parse loop on the captured string with a different rule as start. Same grammar, fresh ignore-stack, fully recursive. Used by the Tcl grammar to re-parse quoted strings for `$var` and `[cmd]` substitutions.
- **Rule-local ignore overrides** — `RULE ignore X Y: …` attaches an ignore-list override to a rule. The parse driver pushes the override on rule entry, pops on exit. Rules without an override inherit the caller's active ignore. Combined with subparse, makes multi-context grammars (script + embedded expression + token internals) a single-file artefact.
- **`*` raw-consume primitive** — `*:body=@, "STOP" newline` scans bytes until a literal sibling matches, bypassing the ignore-set. Vendor-extension bodies and other opaque content round-trip byte-for-byte. The loader and lint both reject a `*` not followed by a literal sibling in the same sequence.
- **List-append binding** — `:name[]=@` captures multi-instance clauses losslessly without giving up the catcher-flatten convenience for single-instance ones.

## Value model

- Typed AST: `null` / `bool` / `int` / `uint` / `real` / `string` / `array` / `dict`.
- Primitive interning and back-references for post-parse value search.
- The dict shape is deterministic from the grammar — see `rawast schema <grammar>` for the data-shape reference.

## Python developer surface

- **`Grammar` class** with `parse_string` / `parse_file` / `parse_bytes` / `save` / `lint`. Bundled grammars addressable by short name (`Grammar("json")`, `Grammar("lefdef")`, etc.). Load your own with `Grammar.load("path/to/file.rawast")`.
- **`start="RULE"` parameter** on all parse and save methods — lets one grammar carry multiple top-level rules (LEF + DEF in `lefdef.rawast`), and lets sub-language entry points be reached directly from host code.
- **Pydantic v2 model generator** — `rawast pydantic <grammar>` emits a ready-to-import Python module whose classes mirror the grammar's parse/save dict shape exactly. Round-trip contract: `Class.model_validate(g.parse_file(p)).model_dump(...)` equals the parsed dict. The model rejects any field the grammar can't save back (`ConfigDict(extra="forbid")`, `populate_by_name=True`). Discriminated unions, list-append bindings, nested sub-dicts, and forward references all flow through to typed Python fields.
- **Python source-code generator** — `rawast pycode <grammar> <file>` reads any real input file and emits Python source that constructs the equivalent Pydantic model. Typed fields with autocomplete and validation; the output is a runnable Python module that reconstructs the model and (optionally) saves it back to the original format. Useful for test fixtures, tutorials, and programmatic editing.
- **Schema documentation generators** — `rawast docs <grammar>` produces an EBNF-flavoured Markdown reference; `rawast schema <grammar>` produces a value-tree-shape reference (dict / array / choice — what a producer tool builds before `save()`).

## Performance

- **First-byte peek-and-skip parse optimization.** The parse loop precomputes a per-Node first-byte set at grammar-load time (Choice union, Sequence first-non-nullable, Repeat item, Ref chase, known-std-parser sets for `int`/`uint`/`float`/`string`) and uses it to skip optional Refs and Choice alternatives whose first byte can't match the input cursor — no frame push, no parser dispatch, no stream rewind. On the 263-file local LEF corpus this trimmed total parse time from **75.2 s → 60.9 s (−19%)**; the Sky130 tech LEF alone dropped from **12.5 ms → 6.3 ms (−50%)**. Conservative fallback: anywhere the analysis can't prove a miss (Parse / Raw / cyclic Ref chains), the optimization yields and the original push-and-try path runs.
- **O(1) `resolve_ref` cache on save.** Precomputed at first `Grammar::save` call. The save engine is now consistently **0.35–0.78× parse time** on DEF files (down from 0.56–1.47× before the cache). Same dict, same output, ~45% faster.
- **RTTI-free Value narrowing.** `as_string` / `as_array` / `as_dict` / `as_int` helpers do a single virtual `type()` call then `static_pointer_cast`, avoiding the RTTI chain walk of `dynamic_pointer_cast`. Used throughout the save engine.

## Per-rule profiling

- **`rawast parse --profile`** prints a top-N table of rules sorted by inclusive parse time, with per-rule entry count, fail count, and deepest stack depth. `--profile-top=N` controls table size (`all` for full).
- **`rawast profile`** aggregates the same counters across a corpus — `<files…>` or `--from-file LIST` — sorted by `time` / `count` / `fails`. The grammar linter and the engine both emit into the same counter pool so the profile and per-rule dispatch numbers match.

## Test suite

253 tests in two layers:

- **208 C++ doctest** covering the engine, loader, JSON round-trip, GDSII round-trip, linter, pretty-print, the `use:` directive, subparse, per-rule ignore, list-append binding, the data-shape schema generator.
- **45 Python pytest** covering Pydantic generator round-trip on a synthetic full-LEF-spec fixture plus four real Sky130 PDK files (the HD tech LEF, an OpenRAM SRAM, the `top_xres4v2` IO pad, and a multi-ANTENNA HD cell).

Plus 3,132 real production files across GDSII / LEF / DEF / Tcl parsing 100% end-to-end, of which a 1,013-file LEF + GDSII subset additionally `parse → save → reparse`s to a structurally equivalent value tree (1,013 / 1,013).

## See also

- [`EXAMPLES.md`](EXAMPLES.md) — worked examples per capability
- [`AGENTS.md`](AGENTS.md) — using rawast with LLM tools and agents
- [`GRAMMARS.md`](GRAMMARS.md) — shipped grammars + corpus details
- [`CLI.md`](CLI.md) — full CLI reference with examples
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — engine internals: parser groups, `use:`, ignore policy
- [`ROADMAP.md`](ROADMAP.md) — what's planned for 1.0
