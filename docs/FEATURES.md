# Features

What rawast can do today. Most features were validated against a 3,132-file production corpus across four formats; see [`GRAMMARS.md`](GRAMMARS.md) for the per-grammar corpus numbers.

## Engine

- **Predictive PEG parsing** with always-on alt-failure recovery on Choice frames. Each Choice alternative attempt is wrapped in input-cursor `mark()` / `reject()` so partial alt failures restore position and try the next alternative — standard PEG ordered-choice semantics, no opt-in.
- **Structural linter** with four design-time checks: LL(k) lookahead ambiguity (depth-4, sees through shared-prefix Choices that diverge at later Keys), prefix-collision (catches the canonical `"not"` matching the prefix of `"notch"` bug at design time), wildcard-rule-with-nested-Choice-type-emit anti-pattern (breaks save dispatch), and `*` raw-consume misuse. Invoke via `rawast lint <grammar>` or `Grammar.lint()`. Issues identify the offending rule by name with one consolidated message per Choice.
- **Strict-key literal `'X'`** — word-bounded `Key` matching (requires non-word character or EOF after the literal). Eliminates the byte-prefix footgun where `"not"` silently consumes the prefix of `"notch"`. Opt-in via single-quote DSL syntax (`'token'`) or `{"strict": true}` JSON form. Lint catches non-strict prefix shadowing at design time.
- **`repeat+N` quantifier** — extends the existing `repeat+` (min=1) shorthand to arbitrary minimum counts: `repeat+2 <X>` requires at least 2 iterations, `repeat+5 <X>` at least 5, etc. Useful for cardinality constraints — binary operators, EDA spec minimums.
- **Bidirectional walk** — parse and save share one grammar definition. The save direction uses a stack-navigation walk with key-based Choice dispatch, wrapped-substructure descent, and catch-all alternatives. The `.rawast` meta-grammar can save its own parsed grammars back as canonical `.rawast` text (self-host save).
- **Pretty-print attributes** — `indent`, `tab`, `space`, `newline`, `tail="…"` plus a `pretty=true/false` toggle let one grammar cover both compact and human-readable output.
- **Subparse (bidirectional)** — `:subparse="<RULE>"` on a Parse- or Raw-terminal item re-invokes the parse loop on the captured string with a different rule as start. On save, the structured sub-tree is serialized back through the subparse rule first to recover the textual form, then written through the underlying terminal's unparse. Same grammar, fresh ignore-stack, fully recursive, round-trips end-to-end. Used by the Tcl grammar to re-parse quoted strings for `$var` and `[cmd]` substitutions.
- **Rule-local ignore overrides** — `RULE ignore X Y: …` attaches an ignore-list override to a rule. The parse driver pushes the override on rule entry, pops on exit. Rules without an override inherit the caller's active ignore. Combined with subparse, makes multi-context grammars (script + embedded expression + token internals) a single-file artefact.
- **`*` raw-consume primitive** — `*:body=@, "STOP" newline` scans bytes until a literal sibling matches, bypassing the ignore-set. Vendor-extension bodies and other opaque content round-trip byte-for-byte. The loader and lint both reject a `*` not followed by a literal sibling in the same sequence.
- **List-append binding** — `:name[]=@` captures multi-instance clauses losslessly without giving up the catcher-flatten convenience for single-instance ones.

## Value model

- Typed AST: `null` / `bool` / `int` / `uint` / `real` / `string` / `array` / `dict`.
- Primitive interning and back-references for post-parse value search.
- The dict shape is deterministic from the grammar — see `rawast schema <grammar>` for the data-shape reference.

## Python developer surface

- **`Grammar` class** with `parse_string` / `parse_file` / `parse_bytes` / `parse_stream` / `save` / `lint`. Bundled grammars addressable by short name (`Grammar("json")`, `Grammar("lefdef")`, etc.). Load your own with `Grammar.load("path/to/file.rawast")`.
- **`Stream` class** (v0.1.9) — `rawast.Stream` is the canonical parser-input type. Build via `Stream.from_string(...)` / `Stream.from_file(...)`, consume via `g.parse_stream(stream)`. The string / file / bytes overloads above are sugar that builds a Stream internally. Hold a Stream when you want to compose stages of a pipeline (e.g. preprocess once, parse with several grammars) without re-reading.
- **`start="RULE"` parameter** on all parse and save methods — lets one grammar carry multiple top-level rules (LEF + DEF in `lefdef.rawast`), and lets sub-language entry points be reached directly from host code.
- **`Preprocessor` three-mode API** (v0.1.9) — beyond the one-call `pp.process(src) -> str` shortcut, the pipeline splits at every joint: `pp.parse(src) -> AST` is pure parse (no expansion, no state mutation — useful for inspecting `\`define / \`include / \`ifdef` structure); `pp.preprocess(ast, src) -> Stream` runs the walker and returns expanded bytes as a `Stream` you can feed directly into any host grammar via `g.parse_stream(...)`. State (macros, included_files, warnings, source spans) accumulates on the `Preprocessor` instance across calls.
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

352 tests in two layers:

- **241 C++ doctest** covering the engine, loader, JSON round-trip, GDSII round-trip, linter (LL(k) ambiguity, prefix-collision, shared-leading-ref exponential trap, wildcard-Choice anti-pattern, raw-consume), pretty-print, strict-key parse+save, `repeat+N` quantifier, the `use:` directive, subparse (bidirectional), per-rule ignore, list-append binding, Repeat per-iteration rollback, first-byte propagation through optionals, the data-shape schema generator.
- **111 Python pytest** covering Pydantic generator round-trip on a synthetic full-LEF-spec fixture plus four real Sky130 PDK files (the HD tech LEF, an OpenRAM SRAM, the `top_xres4v2` IO pad, and a multi-ANTENNA HD cell), plus 56 SystemVerilog tests covering the comprehensive SV-1800 coverage (preprocessor, OOP idioms, SVA, coverage, clocking, randomization, structured AST shapes, multi-port shorthand, virtual interfaces, class type parameters, etc.).

Plus 3,132 real production files across GDSII / LEF / DEF / Tcl parsing 100% end-to-end, of which a 1,013-file LEF + GDSII subset additionally `parse → save → reparse`s to a structurally equivalent value tree (1,013 / 1,013).

## See also

- [`EXAMPLES.md`](EXAMPLES.md) — worked examples per capability
- [`AGENTS.md`](AGENTS.md) — using rawast with LLM tools and agents
- [`GRAMMARS.md`](GRAMMARS.md) — shipped grammars + corpus details
- [`CLI.md`](CLI.md) — full CLI reference with examples
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — engine internals: parser groups, `use:`, ignore policy
- [`ROADMAP.md`](ROADMAP.md) — what's planned for 1.0
