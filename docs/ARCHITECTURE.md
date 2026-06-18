# Architecture

Concepts a grammar author or engine contributor needs to understand. For the `.rawast` DSL syntax see [`rawast-format.md`](rawast-format.md). For the user-facing feature list see [`FEATURES.md`](FEATURES.md).

## Engine overview

rawast is a **predictive PEG parser** with always-on alt-failure recovery on Choice frames (each alternative attempt is wrapped in input-cursor `mark()` / `reject()` so partial alt failures restore position and try the next alt). The grammar is a tree of typed nodes (`Choice`, `Sequence`, `Repeat`, `Key`, `Parse`, `Value`, `Raw`, `Ref`). The same tree drives both the parse direction (text → value tree) and the save direction (value tree → text).

**Stable invariants:**
- One grammar definition = one parser AND one saver. There is no separate "writer grammar" or "save schema".
- The value tree is JSON-shaped (`null` / `bool` / `int` / `uint` / `real` / `string` / `array` / `dict`). No format-specific types.
- The host never injects parsers, ignores, or schema. The grammar declares everything it needs via `use:` and per-rule `ignore`.

These invariants are load-bearing for the `.jast` container model (M2): a grammar header + value tree is enough to round-trip indefinitely, with no implicit host configuration.

## Parser groups, `use:`, and ignore policy

Every grammar declares the parsers it needs and attaches the ignore policy to whichever rule should be the default-active scope — typically the start rule. The host loader never injects parsers or ignores implicitly.

### `use:` — terminal parser groups

`use:` is a list of parser-group names. In `.rawast` form, comma-separated bare identifiers (`use: std`, `use: std, gdsii`); in JSON form, a JSON array (`"use": ["std", "gdsii"]`). Each named group is registered globally at process start; `use:` makes its parsers addressable in the grammar.

Each parser is addressable under **two names**: bare (`int`) and qualified (`std.int`). Bare works when unambiguous; qualified is self-documenting and disambiguates across groups that share local names. Both forms resolve to the same parser.

**Shipped groups:**

| Group | Parsers |
|---|---|
| `std` | `int`, `uint`, `float`, `identifier`, `qualified_identifier`, `string`, `whitespace`, `line_comment`, `block_comment` |
| `gdsii` | All 47 GDSII record-type parsers (`header`, `bgnlib`, …, `endmasks`) — bare or `gdsii.header` form |
| `lefdef` | LEF/DEF-specific `identifier` (hyphens, slashes accepted) and `line_comment` (`#`-to-EOL). The LEF `BEGINEXT … ENDEXT` vendor-extension body is captured via the grammar-level `*` raw-consume primitive (see [`rawast-format.md §4.5a-1`](rawast-format.md)), not a custom terminal parser. |
| `tcl` | Tcl terminals modelled on Dodekalogue rules — `hspace`, `newline`, `comment`, `brace_group`, `quoted_string`, `bracket_sub`, `bare_word`, `expand_marker`, `var_name`, `escape`, `literal_run`. The `$arr(idx)` array-index body is captured via the grammar-level `*` primitive, not a custom `until_paren` terminal. |

### `RULE ignore PARSER1 PARSER2 …` — rule-local ignore overrides

A per-rule attribute declaring the ignore list for that rule's sub-tree. The parse driver maintains an ignore-stack; on rule entry an explicit override is pushed, on exit popped. Rules without the attribute inherit the caller's active ignore. Empty list (`RULE ignore: …`) means "ignore nothing" — useful for token-internal contexts where whitespace is part of the data.

Examples:

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
# grammars/tcl.rawast — multi-context grammar with subparse + rule-local ignore
use: std, tcl
start: <SCRIPT>
SCRIPT ignore tcl.hspace: sequence dict { ... }
WORD_SEGMENTS ignore: sequence dict { ... }    // override to ignore nothing
```

The in-memory `make_json_grammar()` (C++) is **JSONC by construction** — it applies the `std` group internally and adds whitespace + comments to its ignore list. This is the bootstrap grammar used to read JSON-form grammar files (which typically carry inline `//` and `/* */` docs).

## Subparse — `:subparse="<RULE>"`

A terminal parser captures a chunk of input as a string. With `:subparse="<RULE>"` on the binding, the engine immediately re-invokes the parse loop on the captured string with `<RULE>` as the start rule. Same grammar, fully recursive.

Used by the Tcl grammar to handle word substitutions:
- `tcl.quoted_string` captures `"foo $bar [baz]"` as one string.
- `:subparse="WORD_SEGMENTS"` re-feeds that string through the parser with `WORD_SEGMENTS` as start.
- `WORD_SEGMENTS` recognizes variable references (`$bar`), bracket substitutions (`[baz]`), escapes, and literal runs.
- For bracket subs, the body is itself a Tcl script — `BRACKET_SEG` uses `:subparse="SCRIPT"` to recursively parse it as a full Tcl script.

**Self-contained re-entry.** A `:subparse` re-parse starts with a **fresh `ignore_stack`** — it does NOT inherit the surrounding context's ignore policy. The design intent: each `:subparse` target is its own self-contained mini-language (Tcl SCRIPT, sv_pp_expr PP_EXPR, lefdef sub-grammars) that declares its own ignore rules and processes the captured bytes on its own terms. This is **distinct** from the scope-INNER subparse path, which IS inheriting (see "scope INNER inheritance" below) — INNERs run *inside* the surrounding scope's parse, sharing context; `:subparse` re-parses are a *fresh top-level* on the captured bytes.

A practical consequence: the subparse rule typically declares its own `ignore`. The captured body may carry leading/trailing whitespace from the surrounding scope, and the subparse rule's own ignore policy is what skips it. If the subparse rule has no `ignore` declaration, the captured whitespace is content and must be matched literally.

The braced `{…}` case is structurally different: the grammar captures the brace body as a literal, no automatic subparse. The application re-parses it on demand by calling `g.parse_string(body, start="SCRIPT")` from host code — exactly how Tcl's runtime evaluates `if { … }` and `proc { … }` bodies.

## Scope INNER inheritance

When a `scope { OPEN, INNER..., CLOSE }` form dispatches a non-leaf INNER rule (a `<Ref>` to another rule), the INNER's parse runs as a **subparse from within `walk_scan`**. Unlike `:subparse="<RULE>"`, this subparse **inherits** the surrounding scope's ignore policy:

- The caller's active ignore set is seeded onto the subparse's `ignore_stack` (entry-depth=0, persists for the whole subparse).
- A first-content guard suppresses `run_ignore` at Key/Parse/Raw/scope sites until the INNER consumes its first byte of content — keeping leading whitespace in the surrounding scope's body capture instead of being absorbed into the INNER's span.
- End-of-parse `run_ignore` is skipped for non-full-consume re-parses, preserving trailing-whitespace byte attribution the same way.

Result: an INNER rule with no explicit `ignore` declaration behaves identically whether dispatched via a normal `<Ref>` or as a scope INNER. The asymmetric "works standalone but fails inside a scope" failure mode is gone.

The contrast with `:subparse`: scope INNERs share context (they're matching bytes *inside* the surrounding rule); `:subparse` re-parses are conceptually a fresh parse of a captured value (a different language interpreting that value).

## The bidirectional walk

The same node tree drives both parse and save:

**Parse direction.** Walk the node tree, consuming input. Each node either consumes a fixed prefix (`Key`), invokes a terminal parser (`Parse`), recurses (`Sequence`, `Choice`, `Repeat`), or emits a value (`Value`). Bindings (`:name=@`, `:name=const`, `:name[]=@`) attach the consumed result to the surrounding value-tree scope.

**Save direction.** Walk the node tree, reading from the value tree. Each node either emits a fixed prefix (`Key`), invokes the terminal parser's unparse method (`Parse`), recurses, or reads a value (`Value`). The save engine uses a stack-navigation walk with:
- **Key-based Choice dispatch** — choose the alternative whose literal-key prefix matches the current value tree's discriminator field.
- **Wrapped-substructure descent** — optional sub-rules with named bindings pull their fields from the surrounding dict scope.
- **Catch-all alternatives** — a Choice alternative without explicit discriminators is tried last.

The save engine is what makes self-hosting work: the `.rawast` meta-grammar can save its own parsed grammars back as canonical `.rawast` text.

See `src/save_stack.cpp` for the engine. The entry point is `Grammar::save` (defined in `save_stack.cpp`, declared in `grammar.hpp`).

## The structural linter

`Grammar::lint()` runs structural checks on the grammar tree at load time. Issues detected:

- **LL(k) ambiguity on Choices.** A Choice whose alternatives share a first-token signature (and whose LL(k) lookahead can't prove disjointness within bounded depth) is flagged. The engine actually handles these cases correctly via always-on alt-failure recovery for Choice frames — the lint flags them as informational design feedback (the alt-failure cost is small but real; restructuring eliminates it). There is no silencer flag; either restructure the alternatives to diverge earlier or accept the warning as a permanent design note in the lint output.
- **Wildcard-rule-with-nested-Choice-type-emit anti-pattern.** A `sequence dict` rule with no top-level `:type=…` discriminator whose body contains a nested Choice with `type=` emits. Save dispatch can't introspect through the nested Choice, so the rule looks like a catch-all and may swallow values destined for sibling alternatives, then fail on the inner Choice with "no matching grammar alternative for value at save". Fix: lift each nested-Choice alt to a sibling rule of the outer Choice.
- **Byte-scan stop sanity (`*` and `scope { ... }`).** Both `*` (Raw) and `scope { … }` need at least one Key literal that bounds them in the surrounding context. Two valid shapes: (1) direct child of a Sequence with the next sibling Key carrying the stop, (2) inside a `repeat ... separator X` whose post-repeat sibling is a Key — both the separator and the post-repeat Key serve as multi-stop boundaries. Anything else (scope wrapped in Choice / optional / nested Repeat) is rejected: the resolver can't determine stops and the engine would error at parse time. The linter mirrors the loader's resolver — the same checks fire at lint time with friendlier diagnostics than the loader's hard error.

The lint runs automatically when grammars are loaded for the test suite, so regressions surface at PR time.

## The save dispatch contract

This bit is what the nested-Choice lint protects:

When save reaches a `Choice` node and needs to pick an alternative, it inspects each alt for a **discriminator** that can be matched against the current value:

1. **`(Value-name X, Value-const C)` adjacent pair** — dict[X] must equal C.
2. **`(Value-name X, Key K with Value-child C)` adjacent pair** — dict[X] must equal C (the Key's emit).
3. **`(Value-name X, Ref-to-Choice-of-emit-keys)` adjacent pair** — dict[X] must equal one of the Choice's literal emits.
4. **First-child Key with literal token** — used for key-based dispatch in open-schema dicts.

The first three are the "this alt handles type=Foo" pattern. If a rule body has none of them at the top level, the rule becomes a **wildcard** at dispatch — it matches any input dict. That's intentional for genuine catch-alls, but it's a bug when the type discriminator was meant to be visible and got hidden inside a nested Choice. The lint catches the bug shape.

## Performance: first-byte peek-and-skip + resolve_ref cache

Two optimizations that materially shape the engine's hot loop:

**First-byte peek-and-skip (parse).** At grammar-load time, the engine precomputes per-Node first-byte sets — the union of every possible input byte that node could begin with. On a tight inner loop (Choice alternative attempt, optional Ref descent), the engine checks the current input byte against the node's first-byte set before doing any push-and-try work. If the byte can't match, the node is skipped without a frame push, parser dispatch, or stream rewind. On the 263-file LEF corpus this is a 19% wall-clock improvement; on the Sky130 tech LEF it's 50%.

**resolve_ref cache (save).** At first `Grammar::save` call, the engine precomputes a per-NodeId resolved-Ref table. `resolve_ref` becomes a vector index lookup instead of a loop with `std::map` string lookups and `dynamic_pointer_cast`. The save engine is now 0.35–0.78× parse time on DEF files (was 0.56–1.47× before).

See [`FEATURES.md`](FEATURES.md) for the numbers. See `src/grammar.cpp` and `src/save_stack.cpp` for the implementations.

## See also

- [`rawast-format.md`](rawast-format.md) — `.rawast` grammar language reference
- [`AGENTS.md`](AGENTS.md) — using rawast with LLM tools and agents (this doc + `rawast-format.md` are the required reading for an agent authoring grammars)
- [`FEATURES.md`](FEATURES.md) — user-facing features
- [`GRAMMARS.md`](GRAMMARS.md) — shipped grammars
- [`CONTRIBUTING.md`](../CONTRIBUTING.md) — how to contribute
