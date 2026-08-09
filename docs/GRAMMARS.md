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
| `sv` | `grammars/systemverilog.rawast` | SystemVerilog (comprehensive SV-1800 structural coverage) | 56 tests + 100% on 69-case real-world sweep | shipped |

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
- One remaining file (a 100 MB+ production-flow output) parses and round-trips cleanly but is skipped from the automated runner for speed.
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

## `sv` — SystemVerilog (comprehensive SV-1800)

`grammars/systemverilog.rawast`. Comprehensive structural coverage of the SystemVerilog IEEE 1800 LRM, well beyond the original V2001 synthesizable subset. Hits 100% on a 69-case real-world pattern sweep covering OOP idioms, statements, expressions, types, top-level declarations, SVA, modports, clocking, covergroups, and an end-to-end UVM-style testbench file.

Emits a direct `{type, ...}` IR via grammar bindings — no post-parse lowering step. Structured AST for typedef enum / struct / union (label and field arrays, not raw bodies), modport entries (typed direction-group records), assertions (kind + body), and every other declaration form.

**What's covered (structural recognition):**

* **Top-level**: module / interface / class / package / program / config / primitive (UDP), nested modules/interfaces, file-level typedef and parameter declarations.
* **Preprocessor**: `` `define ``, `` `include ``, `` `ifdef ``, `` `ifndef ``, `` `undef `` directives + macro use in 8+ syntactic positions (expression, statement, module-item, identifier slots, number-size prefix, function-call args).
* **Ports**: ANSI / non-ANSI / multi-port shorthand (`input clk, req, gnt` with inherited direction) / user-typed (`input data_t din`).
* **Declarations**: net / reg / integer / nettype / genvar / virtual-interface / user-typed / all array kinds (queue `[$]`, bounded `[$:N]`, associative `[type]`, dynamic `[]`, fixed `[N]`) + multi-dim packed (`logic [7:0][3:0] mem`).
* **Types**: typedef with **structured** enum (label array), struct/union (field array), built-in keyword set extended (`string`, `chandle`, `event`, `void`, `realtime`, `shortreal`).
* **Class**: extends with qualified base (`pkg::Base`) and `implements I1, I2`, value + type parameters (explicit + implicit `parameter` keyword forms), virtual / pure-virtual / extern / static / lifetime modifiers, virtual interface reference, OOP primaries (this / super / null / new / pkg::name / method-call).
* **Expressions**: full 14-level precedence chain + `inside` operator + streaming concat (`{>>{...}}`, `{<<N{...}}`) + type casts (identifier, width, signed) + assignment patterns (`'{a: 1, b: 2}`) + method calls with dot-paths + chained array selects.
* **Statements**: blocking/non-blocking assign + 12 compound-assign forms + if/else (with `priority`/`unique`/`unique0` modifiers) + case (with `inside` and uniqueness modifiers) + for (with declaration init, `i++`/`i--` step) + foreach + do-while + while + repeat + forever + fork-join/_any/_none + wait fork / disable fork + randcase / randsequence + return / break / continue + local variable declarations + method call statements + immediate assertions (with optional labels) + concurrent assertions (also work inside always).
* **SVA**: named property/sequence declarations (with formal args), assert/cover/assume/restrict property + cover sequence, immediate and labeled assertion forms.
* **Coverage**: covergroup blocks (raw body — coverpoints, cross, bins, options remain raw for downstream re-parse).
* **Clocking**: named, `default`, and `global` clocking blocks.
* **Modports**: direction-headed groups with inheritance — parsed to structured `{direction, name}` records.
* **Misc module-item**: let, bind, defparam, specify, checker, extern declarations, generate blocks with conditional/case/for forms (gen-for supports genvar init + `i++` step).
* **End labels**: `endmodule : m`, `end : lbl` etc. — captured everywhere.

**Parser group:** `sv` — SV-specific terminals (the rest comes from `std`):
* `sv_identifier` — simple `[a-zA-Z_][a-zA-Z_0-9$]*`, escaped `\<chars-until-whitespace>`, and system `$<simple-identifier>` forms.
* `sv_number` — every Verilog numeric literal form: unsized integer (`42`, `1_000_000`), sized based (`8'hFF`, `4'b0101`), signed (`8'sh80`), x/z/`?` digits (`4'bxxxx`, `8'h??`), reals (`42.5`, `1.5e10`), time literals (`42ns`).
* `sv_balanced_arg` — single macro/method argument with depth-tracked paren balancing (`MACRO(g(a,b), c)` → two args).
* `sv_balanced_braces` — content-until-outermost-`}` parser, tracks `()`/`{}`/`[]` depth. Used for opaque sub-language bodies (constraint blocks) that can contain nested braces.
* `sv_line_text` — line-aware capture for `` `define `` bodies (handles `\` line continuation).
* `std.linespace` — 0+ horizontal whitespace, never crosses newlines. Used by line-aware preprocessor directives.

Strings and comments use the `std` group's `string`, `line_comment`, and `block_comment` parsers — same surface forms as SV LRM §5.4 and §5.9.

**Real-world verification**: a 60-line UVM-style file containing `package` + `typedef enum` + `typedef struct packed` + parameterized class with `extends`/`rand`/`constraint`/`inside`/method-call/`virtual function`/`extern virtual task`/`new()`/`super.new`/`this.x` assignments + `import` + `module` with class instantiation + `fork`/`join_any` + `global clocking` — all 3 top-level constructs parse, structured AST round-trips through save to 952 bytes.

**Inner sub-languages captured raw** (host re-parses with dedicated passes, OR a future-work subparse target):
* SVA temporal operators (`##`, `|->`, `|=>`, `s_eventually`, `throughout`, `within`, `accept_on`, `reject_on`) inside property bodies.
* Constraint distribution operators (`dist`, `solve`, `before`, `soft`, `unique`) inside constraint blocks.
* Coverpoint / cross / bins items inside covergroup bodies.
* UDP truth table rows.
* Specify timing path expressions.

These are dialect-specific sub-languages best handled by downstream passes; the grammar captures their bodies as `*:body=@` for the host to re-parse on demand.

## Round-trip claims, summarised

| Grammar | Parse | Round-trip | Equivalence type |
|---|---|---|---|
| GDSII | 1,171 / 1,171 | 750 / 750 | byte-equivalent (binary format, full byte preservation) |
| LEF | 507 / 507 | 263 / 263 | structurally equivalent (text format, canonicalized output) |
| DEF | 436 / 436 | 435 / 435 | structurally equivalent (text format, canonicalized output) |
| Tcl | 1,440 / 1,440 | — | parse-only (round-trip not yet measured) |
| SystemVerilog | 56 / 56 (test suite) + 100% on 69-case real-world sweep | UVM-style file round-trips | structurally equivalent (text format, canonicalized output); production-corpus measurement is a future deliverable |

**"Structurally equivalent"** means `parse → save → reparse` yields the same value tree. The output bytes may differ from the input in whitespace, comment positions, or clause ordering where the grammar makes the form irrelevant. **"Byte-equivalent"** means the output bytes match the input exactly.

## Preprocessor grammars — the driver contract

The C++ `Preprocessor` (scan-driven; `src/preprocessor.cpp`) is **grammar-agnostic across the backtick / Verilog family**. It is coupled only to a small contract, not to `systemverilog.rawast` — any backtick-triggered grammar that satisfies it drives the same engine unchanged. (`systemverilog.rawast` is one such grammar; `tests/backtick_pp.rawast` is a second, minimal one that exists to prove the decoupling and guard it in CI.)

The contract has three parts:

**1. Two rule names (by convention):**

| Rule | Role |
|---|---|
| `PP_CONSTRUCT` | The per-backtick construct entry — a `choice` of the directive / macro-use alternatives. The scan driver runs `parse_from(PP_CONSTRUCT)` at each `` ` ``. |
| `MACRO_BODY` | Segments a stored raw macro body on first expansion. Shape: `sequence dict { scope array { … }:segments=@, "\n" }` — the trailing `"\n"` is the sentinel stop. |

**2. A `:type="…"` role tag** on each construct, which the driver dispatches on. The vocabulary is **universal, not SV-specific**: `define`, `undef`, `include`, `ifdef`, `ifndef`, `pp_if`, `elsif`, `else`, `endif`, `macro_use` (plus body-segment tags `ref`, `string`, `macro_use`, `stringify`, `token_paste`). Dispatch keys on the role *value*, not the keyword spelling — the fixture uses `` `def `` (not `` `define ``) and still expands.

**3. Value shapes** the driver reads:

```
define    → { type, decl:{ name, params:[{name, default?}] }, body:[raw text / LINE_CONT] }
macro_use → { type:"macro_use", name, args?:[raw] }
segments  → [ {type:"ref",value} | {type:"string",value} | {type:"macro_use",…} | raw text ]
```

What is **not** generic: the scan driver hardcodes the backtick trigger and the `"` / `//` / `/*` pass-through lexis. A `#`-directive, bare-identifier-macro shape (C/C++/M4) would need scan-model changes and is out of scope. See `tests/backtick_pp.rawast` for a complete worked example and `tests/test_preprocessor.cpp` (`generic driver: …`) for the regression that enforces this contract.

## Adding a new grammar

Write a `.rawast` (DSL) or `.json` (data) file using the conventions in [`rawast-format.md`](rawast-format.md). Drop it in `grammars/`. Load it with `Grammar.load("grammars/myformat.rawast")` or address by short-name if you also stage a copy under `python/rawast/grammars/`.

Required reading for grammar authors: [`rawast-format.md`](rawast-format.md) and [`ARCHITECTURE.md`](ARCHITECTURE.md).

## See also

- [`EXAMPLES.md`](EXAMPLES.md) — worked examples per grammar
- [`rawast-format.md`](rawast-format.md) — `.rawast` grammar language reference
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — parser groups, ignore policy, subparse
- [`CLI.md`](CLI.md) — running the bundled grammars
- [`FEATURES.md`](FEATURES.md) — engine capabilities
