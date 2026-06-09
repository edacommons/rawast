# Command-line reference

All commands are subcommands of `rawast`. Run `rawast --help` for the list, `rawast <command> --help` for per-command options.

## `rawast parse <grammar> <file>`

Parse a file through a grammar; emit the resulting AST as JSON on stdout.

```sh
rawast parse grammars/json.json file.json
rawast parse grammars/lefdef.rawast sky130_fd_sc_hd.lef
rawast parse grammars/gdsii.rawast layout.gds > layout.json
```

**Options:**
- `--pretty` — indent the JSON output for human reading. Default is compact.
- `--start RULE` — pick a non-default top rule. For grammars with multiple entry points (e.g. `lefdef.rawast`'s `LEF` / `DEF`), required if the default isn't what you want.
- `--profile` — enable per-rule parse-time profiling; print a top-20 table on stderr after the parse finishes.
- `--profile-top N` — limit the profile table to top-N rules. Use `--profile-top all` for the full list, no truncation.

**Profiling example:**
```sh
rawast parse grammars/lefdef.rawast sky130_fd_sc_hd.lef \
    --profile --profile-top 30 > /dev/null
# stderr shows top 30 rules by inclusive parse time:
#   rule                                  calls     fails             ns    pct  depth
#   ----------------------------------------------------------------------------------
#   LAYER_BODY_ITEM                       42184         0    18,201,512   24.1%     7
#   MACRO_BODY                            12044         0    12,331,805   16.3%     9
#   ...
```

## `rawast save <grammar> <input.json>`

Read a JSON-described value tree from `<input.json>` and save it through the grammar to stdout (binary-safe).

```sh
rawast save grammars/json.json ast.json > out.json
rawast save grammars/gdsii.rawast layout.json > layout.gds   # binary output
```

**Options:**
- `--pretty` / `--no-pretty` — toggle the grammar's pretty-print pass. Defaults to on for human-readable text grammars; binary grammars (GDSII) ignore it.

## `rawast convert --read <grammar1> --write <grammar2> <file>`

Parse `<file>` with one grammar, then save through another. Useful for `<format>` → JSON conversions.

```sh
# GDSII binary → JSON text
rawast convert --read grammars/gdsii.rawast \
               --write grammars/json.json \
               layout.gds > layout.json

# .rawast → JSON form (round-trip self-host check)
rawast convert --read grammars/rawast.rawast \
               --write grammars/rawast.json \
               grammars/lefdef.rawast > lefdef.json
```

## `rawast lint <grammar>`

Run the structural linter on a grammar. Outputs human-readable diagnostics; exit code is 0 if clean, non-zero if issues found.

Detects:
- **LL(1) violations** — Choices whose alternatives share a first-token signature, on non-`backtrack` Choices.
- **Wildcard-rule-with-nested-Choice-type-emit anti-pattern** — a sequence-dict rule with no top-level `:type=…` discriminator whose body contains a nested Choice with type emits. Such rules act as wildcards at save dispatch and silently swallow types meant for sibling alts.
- **Raw-consume (`*`) misuse** — `*` not followed by a literal-key sibling in the same sequence.

```sh
rawast lint grammars/gdsii.rawast
# clean → exit 0, no output

rawast lint grammars/lefdef.rawast
# any issues print as paragraphs naming the rule and the problem
```

## `rawast profile <grammar> <files…>`

Parse a corpus of files; aggregate per-rule profiling stats across all of them.

```sh
# Per-file enumeration
rawast profile grammars/lefdef.rawast file1.lef file2.lef file3.lef

# Files listed in a manifest
rawast profile grammars/lefdef.rawast --from-file lef_list.txt
```

**Options:**
- `--from-file FILE` — read input paths from `FILE`, one per line. Lines starting with `#` are treated as comments and skipped.
- `--ext EXT[,EXT…]` — when an argument is a directory, glob for files with these extensions. Default: `rawast`.
- `--by {time|count|fails}` — sort key for the aggregate table. `time` sums inclusive ns per rule; `count` totals entries; `fails` totals fail counts.
- `--top N` — show top-N rules. `all` for full table.

```sh
rawast profile grammars/lefdef.rawast --from-file lef_corpus.txt \
    --by time --top 50
```

## `rawast docs <grammar>`

Emit an EBNF-flavoured Markdown reference for the grammar's *input* syntax on stdout. Suitable for committing as project documentation.

```sh
rawast docs grammars/gdsii.rawast > docs/gdsii-syntax.md
```

**Options:**
- `--heading-level N` — base heading level (default 1, so top heading is `#`).

## `rawast schema <grammar>`

Emit a Markdown reference for the grammar's *value-tree* shape (the dict / array / choice structure a producer constructs before calling `save`). The output is the inverse of `docs`: what a host tool builds, not what a user types.

```sh
rawast schema grammars/lefdef.rawast > docs/lefdef-data-shape.md
```

**Options:**
- `--title TITLE` — override the auto-derived title.
- `--heading-level N` — same semantics as `docs`.

## `rawast pydantic <grammar>`

Generate a Pydantic v2 module whose models mirror the grammar's parse/save dict shape exactly. Output goes to stdout; redirect to a file you can `import`.

```sh
rawast pydantic grammars/lefdef.rawast > rawast_models.py
```

**Options:**
- `--module-doc DOC` — override the module docstring.

**Round-trip contract:**
```python
import rawast, rawast_models
g = rawast.Grammar("lefdef")
ast = g.parse_file("sky130_fd_sc_hd.lef", start="LEF")
model = rawast_models.LEF.model_validate(ast)
dumped = model.model_dump(exclude_none=True, by_alias=True)
assert ast == dumped                          # round-trip guarantee
assert g.save(dumped, start="LEF") is not None
```

## `rawast pycode <grammar> <file>`

Read a parsed file, validate it against the Pydantic model, and emit Python source that reconstructs the equivalent typed model. Output is a runnable module: when executed, it builds the same model and (optionally) saves it back to the original format.

```sh
rawast pydantic grammars/lefdef.rawast > models.py
rawast pycode grammars/lefdef.rawast sky130_fd_sc_hd.lef \
    --start LEF --models-module models > fixture.py

# Run the generated module to regenerate the original file:
python fixture.py > sky130_fd_sc_hd.regenerated.lef
```

**Options:**
- `--start RULE` — top rule for multi-top-rule grammars. Required for `lefdef.rawast` (`LEF` or `DEF`).
- `--models-module MODULE` — Python import path to the Pydantic-models module. If omitted, models are generated and exec'd inline (the emitted code's `from … import *` will need a real module to import from).

**Use cases:**
- Test fixtures — convert a real corpus file into self-contained Python that builds the same AST.
- Tutorials — readable typed code mirroring a real LEF/DEF/GDSII.
- Programmatic editing — start from a real file, edit in your IDE with autocomplete + Pydantic validation, save back through rawast.

**Limitations:**
- Output size grows linearly with input. Multi-MB inputs produce multi-MB Python sources. Practical for files <~5 MB; impractical for production-scale outputs.
- Repetitive content emits N separate constructor calls — no loop / comprehension synthesis.

## `rawast --help` / `rawast --version`

Standard. `--version` prints the engine version (matches `rawast.__version__` and `pyproject.toml`).

## See also

- [`EXAMPLES.md`](EXAMPLES.md) — worked Python + CLI examples per capability
- [`FEATURES.md`](FEATURES.md) — what each command exposes
- [`GRAMMARS.md`](GRAMMARS.md) — bundled grammars by short name
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — parser groups, `use:`, ignore policy
