# Using rawast with LLM agents

You're going to write a `.rawast` grammar. The engine loads it at runtime, parses input to a JSON-shaped AST, and saves the AST back to text. Same grammar, both directions. No code generation, no recompile.

This document is the quick reference. The five idioms below cover ~90% of grammar authoring.

## Read first

1. **[`rawast-format.md`](rawast-format.md)** — DSL spec (Choice, Sequence, Repeat, Key, Parse, Value, Raw, Ref, Scope, bindings, list-append `:name[]=@`, raw-consume `*`, annotation marks `#opchain` / `#subparse` / `#role`). Required.
2. **[`ARCHITECTURE.md`](ARCHITECTURE.md)** — engine internals + lint anti-patterns.
3. **Reference grammars** in [`../grammars/`](../grammars/):
   - `json.json` — minimal first example
   - `tcl.rawast` — `subparse` for embedded sub-languages
   - `lefdef.rawast` — full-scale grammar with multi-top entries
   - `systemverilog.rawast` — precedence ladder with `#opchain` (COND_EXPR), scope-array for segmented macro bodies, and the merged preprocessor (PP_FILE)

## The five idioms

### 1. `sequence dict` with field bindings → structured record

```rawast
DEFINE: sequence dict {
  "`define":type="define",
  linespace,
  identifier:name=@,
  ?<PARAMS>:params=@,
  "\n"
}
```

`:field=@` binds the value into a dict field. `:type="define"` is a const binding. The rule's AST is the application's IR shape — emit it directly.

### 2. `choice` with discriminators → tagged variants

```rawast
PP_ITEM: choice { <DEFINE>, <UNDEF>, <INCLUDE>, <IFDEF>, <IF>, <TEXT_LINE> }
```

PEG ordered alternatives; first match wins. **Every Choice alternative needs a distinct discriminator field** (`type=`, `op=`, `kind=`) — save dispatch fails silently otherwise.

### 3. `scope` and `scope array` → segmented content with a stop

The single most under-used idiom. When you need mixed text + structured segments inline (macro body with `\`NAME` calls; text line with embedded markers; paren-balanced expression body), reach for `scope { ... }` (single value) or `scope array { ... }` (array of mixed text + typed segments).

**Stop comes from the sibling Key after the scope** in the surrounding sequence — no `start=`/`stop=` attributes.

```rawast
TEXT_LINE: sequence dict {
  scope array { <MACRO_USE> }:segments=@,
  "\n":type="text_line"
}
```

Produces `{type:"text_line", segments:[<text>, {type:"macro_use", name:"FOO"}, <text>, …]}`. Walker iterates segments; INNERs dispatch by `type`.

**Do not reach for `*` + `:subparse=` for this.** See decision table below.

### 4. `#opchain` on precedence ladders → clean `{op, args}`

Write the always-wrap shape, mark the top rule `:#opchain`. Engine compacts to uniform `{op, args}` post-parse. No `lower()` step.

```rawast
PP_EXPR: <OR_EXPR>:#opchain

OR_EXPR: choice { <OR_CHAIN>, <AND_EXPR> }
OR_CHAIN: sequence dict {
  <AND_EXPR>:lhs=@,
  repeat+ <OR_TAIL>:tail[]=@
}
OR_TAIL: sequence dict { '||':op="||", <AND_EXPR>:rhs=@ }

AND_EXPR: choice { <AND_CHAIN>, <NEXT_LEVEL> }
// ... same shape per level
```

`parse("a || b && c")` returns `{op:"||", args:[{ref:"a"}, {op:"&&", args:[{ref:"b"}, {ref:"c"}]}]}`. Same-op runs collapse n-ary; mixed-op boundaries nest. Save reverses.

### 5. `#subparse` for genuine sub-language nesting

Captured text re-parsed through a different start rule. Use **only** when the inner content is a different sub-language (different ignore, different operators, different lexical structure):

```rawast
PAREN_EXPR: sequence dict {
  "(":type="paren",
  scope { <PAREN_EXPR>, std.string }:value=@:#subparse="PP_EXPR",
  ")"
}
```

Bidirectional. Save routes through the subparse rule on the way back to text.

## scope-array vs `#subparse` — the decision

| Use `scope array { INNER… }` when | Use `*:body=@:#subparse="X"` when |
|---|---|
| Mixed text + typed segments inline | Inner is a genuinely different sub-language |
| Inner shares the outer's ignore + operators | Inner needs its own ignore policy or lexical rules |
| Segments visible in parent AST in document order | Inner is conceptually independent of position |

**The most common agent mistake**: reaching for subparse when scope-array would do. If you can describe the inner with `repeat`/`separator`/`?<X>`/`<EXPR>` using the same grammar's existing rules, parse it **directly** — no subparse.

## Annotation marks

`#`-prefixed marks attach to a rule reference or definition and tell the engine to do extra work. Orthogonal to bindings.

| Mark | Where | Effect |
|---|---|---|
| `#opchain` | Top-of-ladder rule reference | Compact always-wrap `{lhs, tail}` into uniform `{op, args}` post-parse, same-op runs collapsed, mixed-op boundaries nested. Save reverses. |
| `#subparse="RULE"` | Captured-text binding (scope or `*`) | Re-parse captured bytes through `RULE`. Bidirectional. |
| `#role="name"` | sequence-dict rule definition | Tag for walker dispatch (e.g. preprocessor walker switches on `#role="if"`). Walker-side; not parse-shape. |

## Anti-patterns to avoid

- **Two Choice alternatives sharing a discriminator** (`type="class"` twice) — save dispatch can't tell them apart. Inline both as siblings of the outer Choice, distinguish by a leading Key or const binding.
- **Transparent rules** that consume input without recording it (`sequence { "(", <EXPR>, ")" }` — parens lost). Add `"(":type="paren"` so save knows to emit them.
- **Subparse for plain lists** — use `repeat`/`separator` directly.
- **Verbose precedence AST without `#opchain`** — add the mark.
- **Embedded `start=`/`stop=` on scope** — old form, removed. Stop comes from the sibling Key.
- **`*` without a following literal sibling** — the scan has no boundary. Lint flags this.
- **`linespace` is strict (1+)** — for "optional whitespace here," write `?linespace`.

## Whitespace model

The engine runs `run_ignore` before every Key/Parse match using the active `ignore` policy. Rules declare their policy:

- `RULE ignore X Y: ...` — push `X Y` for this rule's scope.
- `RULE ignore: ...` — push empty (suspend inherited ignore — adjacency strict).
- No directive — inherit caller's policy.

For save-time canonical whitespace, use postfix attributes on items: `"X" space` emits `"X "`, `"X" newline` emits `"X\n"`, `"X" tail=";"` emits `"X;"`. Parse ignores them.

## Validate

**Always lint** after writing. `rawast lint <grammar>` catches LL(1) ambiguity, nested-Choice-type-emit, raw-without-sibling-stop. Feed lint output back to the agent for iteration.

**Diagnose parse failures with `RAWAST_TRACE=1`** — dumps every frame the engine pushed and which alternative failed where. Read from the bottom (deepest failures) up. Don't bisect inputs.

**Test against real files with `rawast pycode`** — generates Python that reconstructs the model from a real instance. Round-trip divergence = grammar gap.

## Starting prompt template

```
Write a rawast grammar for <FORMAT>.
Spec: <paste BNF / vendor doc>.

Use the five idioms:
  1. `sequence dict` with `:field=@` bindings for records.
  2. `choice` with `:type=` discriminators on EVERY alt.
  3. `scope array { INNER… }` for segmented content (NOT subparse).
  4. `#opchain` on precedence ladder top — clean `{op, args}`.
  5. `#subparse` ONLY for genuinely different sub-languages.

Conventions:
  - `use:` parser groups; `start:` rule.
  - Literal: `'token'` (word-bounded) for keywords; `"text"` for punctuation.
  - Emit application IR shape DIRECTLY via bindings — no Python `lower()`.
  - Parse lists with `repeat+ <ITEM>:items[]=@ separator ","` — not subparse.
  - Postfix `space`/`newline` for save-time canonical whitespace.

Mentally walk: lint clean? Save dispatch discriminators distinct? Every byte
the parser consumes also emitted on save (no transparent rules)?

Output ONLY the .rawast text.
```

## When NOT to use rawast

- Unstructured prose with no scope hierarchy.
- ≤50 instances of the format (LLM directly is faster than writing a grammar).
- Lossy summarisation (rawast's strength is faithful round-trip).

## See also

- [`rawast-format.md`](rawast-format.md) — DSL spec (required)
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — engine internals, lint
- [`GRAMMARS.md`](GRAMMARS.md) — shipped grammars
- [`EXAMPLES.md`](EXAMPLES.md) — worked Python examples
- [`CLI.md`](CLI.md) — `rawast lint`, `pydantic`, `pycode`
