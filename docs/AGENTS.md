# Using rawast with LLM agents

For LLM tools and agents that need to work with structured documents — file formats, configs, IRs, proprietary schemas — rawast offers a different substrate than text pattern-matching.

## Why agents care

An agent facing an unfamiliar format usually has two bad options:

1. **Pattern-match the text directly** — regex, string searches, ad-hoc tokenization. Brittle. Breaks on whitespace, edge cases, vendor extensions, format revisions.
2. **Hope the format is in pretraining data** — works for public formats the model has seen (JSON, YAML), unreliable for anything proprietary, impossible to verify per-instance.

rawast offers a third:

3. **Write a grammar for the format.** Composing a structured DSL from a specification is exactly the kind of task LLMs are good at. From then on, every instance of the format parses to a typed AST. The agent reasons over the tree instead of the bytes.

What makes this work:

- **Grammar is data, not code.** A `.rawast` file the engine loads at runtime. No recompilation.
- **Bidirectional.** Same grammar drives parse (text → AST) and save (AST → text). Agents that generate output construct the AST and call save.
- **JSON-shaped AST.** Dicts, arrays, scalars. Walked with normal JSON patterns.
- **Lossless round-trip.** Parse → edit → save preserves structure.
- **Privacy.** MIT licence, no network calls. Vendor formats stay private.

## Required reading

Priority order. Sections 1–2 are mandatory before authoring; the rest are reference.

1. **[`rawast-format.md`](rawast-format.md)** — the grammar DSL specification. Node types (`Choice`, `Sequence`, `Repeat`, `Key`, `Parse`, `Value`, `Raw`, `Ref`, `Scope`), binding syntax, ignore policy, list-append (`:name[]=@`), raw-consume (`*`), and the annotation marks (`#opchain`, `#subparse`, `#role`). Read end-to-end before your first grammar.

2. **[`ARCHITECTURE.md`](ARCHITECTURE.md)** — engine internals. Parser groups, `use:` directive, bidirectional walk, save dispatch contract, lint anti-patterns. Critical for understanding why certain shapes don't round-trip.

3. **Reference grammars in [`../grammars/`](../grammars/)**:
   - `grammars/json.json` — minimal grammar, good first example
   - `grammars/tcl.rawast` — `subparse` for embedded sub-languages
   - `grammars/lefdef.rawast` — full-scale grammar with shared rules, multi-top entries, `*` for opaque vendor blocks
   - `grammars/sv_preprocessor.rawast` — modern segmented-body + text-line idioms with `scope array`
   - `grammars/sv_pp_expr.rawast` — precedence ladder with `#opchain` compaction and `#subparse` for paren bodies
   - `grammars/systemverilog.rawast` — large mixed-content reference

4. **[`GRAMMARS.md`](GRAMMARS.md)**, **[`EXAMPLES.md`](EXAMPLES.md)** — narrative reference.

## The five core grammar idioms

These five patterns cover ~90% of grammar authoring. Reach for them in this order.

### 1. `sequence dict` with field bindings — a structured record

```rawast
DEFINE: sequence dict {
  "`define":type="define",
  linespace,
  identifier:name=@,
  ?<PARAMS>:params=@,
  "\n"
}
```

Each item parses left to right. `:field=@` binds the value into a named dict field. `:type="define"` is a const binding (always emits the same value). The rule's AST is `{type: "define", name: "...", params: [...]}` — the application's IR shape, emitted directly.

### 2. `choice` with discriminators — tagged variants

```rawast
PP_ITEM: choice {
  <DEFINE>, <UNDEF>, <INCLUDE>, <IFDEF>, <IF>, <TEXT_LINE>
}
```

PEG ordered alternatives; first match wins. Each branch's `type=` const binding doubles as the save-side dispatch key. **Always give every Choice alternative a distinct discriminator field** (`type=`, `op=`, `kind=`) — save dispatch fails silently otherwise.

### 3. `scope` and `scope array` — segmented content with a stop

This is the **single most under-used idiom** in agent-authored grammars. When you need:

- A region of mixed text-and-structured-content (a macro body with embedded `\`NAME` calls; a line of text with mid-line macro uses; an expression with embedded `(...)` sub-zones),
- Parens/brackets/braces balanced against the surrounding context,
- An atomic-span capture inside a larger sequence,

reach for `scope { INNER... }` (single value) or `scope array { INNER... }` (array of mixed bare-text + typed-dict segments). **Do not reach for `*` + `:subparse=`** — that's heavier and harder to round-trip.

Modern form: the **stop byte comes from the sibling Key after the scope** in the surrounding sequence. No `start=`/`stop=` attributes.

```rawast
TEXT_LINE: sequence dict {
  scope array { <MACRO_USE> }:segments=@,
  "\n":type="text_line"
}
```

This produces `{type: "text_line", segments: [<text>, {type:"macro_use", name:"FOO"}, <text>, ...]}`. The scope scans bytes until it sees `\n` (the sibling). Each `\`NAME` it encounters dispatches `MACRO_USE`; text between matches is captured as bare `StringValue`. The walker iterates `segments` — strings emit verbatim, dicts dispatch on `type`.

Equally for macro bodies:

```rawast
DEFINE: sequence dict {
  "`define":type="define",
  identifier:name=@,
  ?<PARAMS>:params=@,
  ?linespace,
  scope array { <MACRO_USE>, <STRING>, <PARAM_REF> }:body=@,
  "\n"
}
```

The scope's INNER set is the dispatch table. First-byte sets must be disjoint among INNERs (or PEG-ordered with predictive backoff).

Note the `?linespace` — `linespace` itself is strict (requires 1+ whitespace). Bare `linespace` means "whitespace required here"; for an *optional* whitespace slot, prefix it with `?`. The strict default lets you put `linespace` inside a `choice` or `repeat` without a 0-consume infinite-loop hazard, and follows the "terminal succeeds iff it consumed something" invariant.

### 4. `#opchain` on precedence ladders — clean `{op, args}` from always-wrap

For operator-precedence ladders, write the **always-wrap** shape (each level produces `{lhs, tail:[{op, rhs}, ...]}`) and mark the top rule `:#opchain`. The engine compacts the always-wrap shape into uniform `{op, args}` post-parse. No `lower()` function, no Python post-processor.

```rawast
PP_EXPR: <OR_EXPR>:#opchain          // mark drives the compaction

OR_EXPR: choice { <OR_CHAIN>, <AND_EXPR> }
OR_CHAIN: sequence dict {
  <AND_EXPR>:lhs=@,
  repeat+ <OR_TAIL>:tail[]=@
}
OR_TAIL: sequence dict {
  '||':op="||",
  <AND_EXPR>:rhs=@
}

AND_EXPR: choice { <AND_CHAIN>, <NEXT_LEVEL> }
// ... same shape per level
```

`parse("a || b && c")` returns `{op: "||", args: [{op: "ref", value:"a"}, {op: "&&", args: [{ref:"b"}, {ref:"c"}]}]}`. Same-op runs at one level collapse into n-ary args; mixed-op boundaries nest. The engine reverses on save — no extra glue.

Pre-`#opchain` advice told agents "accept the verbose `{lhs, tail}` AST." That's obsolete. Use `#opchain`.

### 5. `#subparse` for genuine sub-language nesting

When the inner content is a **different sub-language** — different ignore policy, different operators, different lexical structure — capture it as text with `scope` and re-parse it through a different start rule:

```rawast
PAREN_EXPR: sequence dict {
  "(":type="paren",
  scope { <PAREN_EXPR>, std.string }:value=@:#subparse="PP_EXPR",
  ")"
}
```

Scope captures the paren body's bytes (with self-Ref INNER making nested parens transparent, and `std.string` INNER keeping `"...)..."` strings atomic). `#subparse="PP_EXPR"` re-enters the engine at the `PP_EXPR` rule on the captured bytes; save reverses through the same hook.

`#subparse` is bidirectional and doesn't need a separate parse call from the host. The AST stores the typed sub-tree.

## scope vs subparse — the decision

The single most common mistake in agent-authored grammars: reaching for **subparse** when **scope array** would do. The two solve different problems:

| Use `scope array { INNER... }` when | Use `*:body=@:#subparse="X"` when |
|---|---|
| Content is a **list of mixed text and typed segments** | Content is a **different sub-language** |
| INNERs share the outer rule's ignore policy and operators | Sub-language needs its own ignore policy or lexical rules |
| You want segments visible in the parent AST in document order | The inner tree is conceptually independent of position |
| Round-trip is "emit each segment in order" | Round-trip routes through a different start rule |

Examples:

- **Macro body** with embedded `\`NAME` and `"strings"` → `scope array { MACRO_USE, STRING }` — same ignore, in-line interleaving. **Not subparse.**
- **Text line** with mid-line macro uses → `scope array { MACRO_USE }`. **Not subparse.**
- **Comma-separated parameter list** → `repeat+ identifier separator ","`. **Not scope, not subparse.** Direct outer-grammar primitive.
- **Paren expression body** inside an operator chain, where the body should re-enter the expression grammar from the top → `scope { PAREN_EXPR, std.string }:value=@:#subparse="EXPR"`. Subparse, **because** the paren body is the full sub-language, not just a list.
- **Tcl `if { body }` block** where `body` is itself Tcl commands → `scope { ... }:body=@:#subparse="TCL_SCRIPT"`. Subparse.

Heuristic: if you can describe the inner shape with `repeat`/`separator`/`?<X>`/`<EXPR>` using the same grammar's existing rules, use them directly. If the inner is a typed list of varying segments in one ignore policy, use `scope array`. Only reach for `#subparse` when the inner content is a genuinely different sub-language.

## The annotation marks

`#`-prefixed marks attach to a rule reference or rule definition and tell the engine to do extra work. They're orthogonal to bindings.

| Mark | Where | Effect |
|---|---|---|
| `#opchain` | On top-of-ladder rule reference | Compact always-wrap `{lhs, tail}` into uniform `{op, args}` post-parse, with same-op runs collapsed and mixed-op boundaries nested. Save reverses. |
| `#subparse="RULE"` | On a captured-text binding (scope or `*`) | Re-parse the captured bytes through `RULE` as an independent sub-tree. Bidirectional through the same hook on save. |
| `#role="name"` | On a sequence-dict rule definition | Tag the rule with a semantic role the walker dispatches on (e.g. preprocessor walker switches on `#role="if"` vs `"define"`). Walker-side concept, not parse-shape. |

`#opchain` is the one agents most often miss. If your grammar parses operator expressions and you find yourself thinking "I'll fix the AST shape in code," stop and add `#opchain` to the ladder's top rule.

## A starting prompt for grammar authoring

```
You are writing a rawast grammar for the following format: <FORMAT NAME>.

Format specification (study it carefully):
<paste spec, BNF, or vendor reference>

Required reading (already in your context):
- rawast grammar DSL reference (docs/rawast-format.md)
- Engine architecture, lint anti-patterns (docs/ARCHITECTURE.md)
- Reference grammars: grammars/tcl.rawast, grammars/sv_preprocessor.rawast,
  grammars/sv_pp_expr.rawast

Your output is a single .rawast file. Author it using the five core idioms:

1. `sequence dict` with `:field=@` bindings for structured records.
2. `choice` with `:type=` (or `:op=`, `:kind=`) discriminators on every
   alternative — never two alts with the same discriminator.
3. `scope array { INNER... }` for segmented content (mixed text and
   typed segments in one ignore policy). The stop byte comes from the
   sibling Key after the scope in the surrounding sequence.
4. `#opchain` on the top of any operator precedence ladder — the
   always-wrap shape compacts to `{op, args}` automatically.
5. `#subparse="RULE"` ONLY when the inner content is a different
   sub-language. For lists, scope-arrays, or anything you could write
   with outer-grammar primitives, do not subparse.

Conventions:

- Declare `use:` parser groups (likely `std` for text formats).
- Declare `start:` rule.
- Pick the right literal form: `"text"` is byte-prefix, `'text'` is
  word-bounded. Use `'token'` for reserved words; use `"text"` for
  punctuation and intentional prefix captures.
- Emit the application's IR shape DIRECTLY via bindings — never plan
  a Python `lower()` step. If the host wants `{op, args}`, the grammar
  emits `{op, args}`.
- Parse structured lists DIRECTLY with `repeat+ <ITEM>:items[]=@
  separator ","`. Do not capture-and-subparse.
- Use `scope array` for segmented bodies before reaching for subparse.
- Mark scope rules with explicit sibling stop bytes (`"\n"`, `")"`, etc.)
  in the surrounding sequence.
- Add a comment with the spec section number for each non-trivial rule.

Before emitting, mentally walk the grammar through:
1. `rawast lint` — would any rule trigger LL(1) ambiguity or the
   wildcard-Choice-type-emit anti-pattern?
2. Save direction — can each Choice alternative be uniquely identified
   by a discriminator field?
3. Round-trip — does every consumed byte have a corresponding emit on
   save? (Transparent rules that consume `(` without recording it
   break round-trip.)

Output ONLY the .rawast text. No commentary outside it.
```

## Common anti-patterns

These are mistakes the lint catches, or that surface only at save time. Author with them in mind.

**Two Choice alternatives sharing a discriminator.** If `CLASS_DECL_VIRTUAL` and `CLASS_DECL_PLAIN` both emit `type="class"`, save dispatch can't tell them apart. Fix: inline both as siblings of the outer Choice, distinguish by a leading Key (`'virtual'`) or a second const binding. The intermediate Choice level only helps when its alternatives have distinct discriminators.

**Transparent rules.** A rule that consumes structurally significant input but records nothing breaks round-trip:

```rawast
PAREN_EXPR: sequence { "(", <EXPR>, ")" }       // wrong — parens lost
PAREN_EXPR: sequence dict {                       // right — paren shape recorded
  "(":type="paren", <EXPR>:inner=@, ")"
}
```

Apply this wherever a rule eats input that affects output without leaving a trace.

**Subparse for plain lists.** Already covered above — if it's a list, parse it directly with `repeat`/`separator`.

**Verbose precedence AST without `#opchain`.** Already covered above — add `#opchain`.

**Embedded `start="X"`/`stop="Y"` attributes on scope.** Older form, removed. Stop comes from the sibling Key after the scope. Modern grammars don't use these attributes.

**`*` raw-consume without a following literal sibling.** `*` consumes bytes until the next sibling matches. Without a sibling stop, the scan has no boundary. The lint flags this.

## Parsing arbitrary character sequences with pure grammar

When the input has a token no shipped terminal parser handles — a hex run with `x`/`z`/`?` characters, an underscored number, a custom literal form — **try pure grammar first**. Most cases are expressible with two mechanisms.

1. **`RULE ignore:`** (empty list) disables whitespace skipping inside the rule. Adjacent characters must match without intervening whitespace — lex a token as one tight sequence.
2. **Character-level `choice`** of single-char strict keys acts as a character class. Combined with `repeat+` you get a digit run.

```rawast
BASED_DIGITS ignore: sequence array {
  repeat+ <BASED_DIGIT>
}

BASED_DIGIT: choice {
  "0":@, "1":@, ..., "9":@,
  "a":@, ..., "f":@,
  "A":@, ..., "F":@,
  "x":@, "X":@, "z":@, "Z":@, "?":@,
  "_":@
}
```

Use this for hex/binary/alphanumeric runs, format-specific separators, anything "consume run of chars in set X." Write a custom C++ Parse terminal only when (a) you'd need 95+ char alternatives, (b) you need to compute a value inside the token (overflow check, escape decode), or (c) the token is canonical across many grammars and belongs in `std`.

## Diagnose with `RAWAST_TRACE`, not bisection

When a parse fails with `unexpected content after start rule completed (byte N)`, set `RAWAST_TRACE=1` and re-run. The trace dumps every frame the engine pushed, every alternative tried, and the exact failure for each — indented by stack depth. Read from the bottom (deepest failures) upward.

```sh
RAWAST_TRACE=1 python -c "
import rawast
g = rawast.Grammar.load('grammars/foo.rawast')
g.parse_file('failing.fmt')
" 2>&1 | tail -80
```

Pipe through `grep -E 'unwind|FAIL'` for large runs. Zero cost when the env var is unset.

## How an agent uses the parsed AST

```python
import rawast

g = rawast.Grammar.load("my_format.rawast")
ast = g.parse_file("input.fmt")

for item in ast.get("items", []):
    if item.get("type") == "Foo":
        process_foo(item["name"], item["value"])
```

For type-safe extraction, generate Pydantic models from the grammar:

```sh
rawast pydantic my_format.rawast > my_format_models.py
```

```python
import rawast, my_format_models as M

g = rawast.Grammar.load("my_format.rawast")
typed = M.Root.model_validate(g.parse_file("input.fmt"))
# typed.items[0].type, typed.items[0].name — IDE autocomplete, validation.
```

## Bidirectional save

```python
obj = M.Root(items=[
    M.Foo(type="Foo", name="thing", value=42),
    M.Bar(type="Bar", flag=True, label="other"),
])

g = rawast.Grammar.load("my_format.rawast")
text = g.save(obj.model_dump(exclude_none=True, by_alias=True))
```

Round-trip property: `parse → typed model → edit → dump → save` is lossless for structure (whitespace and comment positions may canonicalize).

## Practical tips

**Always lint after generating a grammar.** `rawast lint <file>` catches LL(1) ambiguity, the wildcard-Choice-type-emit anti-pattern, and `*` raw-consume misuse. Feed lint output back into the agent for iteration.

**Test with `rawast pycode` against real files.** Point `rawast pycode` at a real instance to generate Python source that reconstructs the model. If the round-trip diverges, the grammar has a coverage gap. Faster than hand-writing fixtures.

**Use rule-local `ignore` for context shifts.** If part of the format treats whitespace differently (quoted string interior, raw token), declare `RULE ignore:` on that rule. The engine pushes the override on entry, pops on exit.

**Track grammars in git like code.** Version them alongside the format spec. A grammar at v1.0 should produce the same AST for the same file across runs.

**Strict keys (`'token'`) for reserved words.** Catches the byte-prefix bug where `"not"` matches the prefix of `"notch"`. Use `"text"` for punctuation, `'text'` for keywords.

## When NOT to use rawast

- **The format is unstructured.** Free prose with no scope hierarchy is a job for the LLM directly.
- **One-off use.** ≤50 instances of the format → letting the LLM handle each instance is faster than writing a grammar.
- **You need lossy summarization.** rawast's strength is faithful round-trip.

## See also

- [`rawast-format.md`](rawast-format.md) — grammar DSL specification (required)
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — engine internals, lint anti-patterns
- [`GRAMMARS.md`](GRAMMARS.md) — shipped grammars as reference
- [`EXAMPLES.md`](EXAMPLES.md) — worked code examples
- [`FEATURES.md`](FEATURES.md) — engine capability list
- [`CLI.md`](CLI.md) — `rawast lint`, `rawast pydantic`, `rawast pycode` reference
