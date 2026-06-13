# Using rawast with LLM agents

For LLM tools and agents that need to work with structured documents — file formats, configs, IRs, proprietary schemas — rawast offers a different substrate than text pattern-matching.

## Why agents care

An agent facing an unfamiliar format usually has two bad options:

1. **Pattern-match the text directly** — regex, string searches, ad-hoc tokenization. Brittle. Breaks on whitespace, edge cases, vendor extensions, format revisions. Doesn't scale across formats.
2. **Hope the format is in pretraining data** — works for public formats the model has seen (JSON, YAML, common config files), unreliable for anything proprietary or in-house, and impossible to verify per-instance.

rawast offers a third:

3. **Write a grammar for the format.** Composing a structured DSL from a specification is exactly the kind of task LLMs are good at. From then on, every instance of the format parses to a typed AST. The agent reasons over the tree instead of the bytes.

## What makes this work for agents

- **Grammar is data, not code.** A `.rawast` (or `.json`) file the engine loads at runtime. No engine modification, no recompilation, no model fine-tuning. The agent writes the grammar; rawast loads it.
- **Bidirectional.** The same grammar drives parse (text → AST) and save (AST → text). An agent doesn't just *read* a format — it *generates* in the format too, by constructing the AST and calling save. This matters for agents that produce structured output (test fixtures, reports, edited configs).
- **JSON-shaped AST.** Dicts, arrays, scalars. No format-specific types to learn. The agent walks the tree with normal JSON patterns (`obj["field"]`, `obj["items"][0]`), same idioms across every format.
- **Lossless round-trip.** Parse → edit → save preserves structure. An agent reading a configuration file can modify a single field and write it back with the rest of the file's content intact.
- **Privacy.** Nothing about the grammar leaves the agent's environment. rawast is MIT, makes no network calls. Sensitive vendor formats and internal protocols stay private.

## What an agent should read before authoring a grammar

Priority order. Sections 1 and 2 are required; the rest are reference material the agent should be able to look up.

1. **[`rawast-format.md`](rawast-format.md)** — the `.rawast` grammar DSL specification. Canonical reference for node types (`Choice`, `Sequence`, `Repeat`, `Key`, `Parse`, `Value`, `Raw`, `Ref`), binding syntax, ignore policy, subparse semantics, list-append (`:name[]=@`) and raw-consume (`*`). An agent should read this end-to-end before authoring its first grammar.

2. **[`ARCHITECTURE.md`](ARCHITECTURE.md)** — engine internals from a grammar author's perspective. Parser groups (`std`, `gdsii`, `lefdef`, `tcl`), the `use:` directive, rule-local ignore overrides, the bidirectional walk, the save dispatch contract, and the **structural lint anti-patterns**. Critical for understanding *why* certain grammar shapes don't work (especially the wildcard-rule-with-nested-Choice-type-emit trap).

3. **[`GRAMMARS.md`](GRAMMARS.md)** — describes the shipped grammars (JSON, GDSII, LEF/DEF, Tcl, rawast meta). Good reference examples for tactic choices: when to use `Choice` vs `Sequence`, how to model lists, how to handle vendor extensions, how to use `subparse` for embedded sub-languages.

4. **Source grammars in [`../grammars/`](../grammars/)** — the actual files. At minimum the agent should study:
   - `grammars/json.json` — minimal grammar (RFC 8259), good first example
   - `grammars/tcl.rawast` — demonstrates `subparse=` and rule-local `ignore` for embedded sub-languages
   - `grammars/lefdef.rawast` — full-scale grammar with shared sub-rules, multi-top-rule entry points, and the `*` primitive for opaque vendor blocks
   - `grammars/systemverilog.rawast` — large mixed-content grammar (~2000 lines): preprocessor + statement chain + 14-level expression precedence + structured class/interface/package/program + comprehensive SVA / coverage / clocking. Good reference for choice-based dispatch on Choice alternatives sharing leading rules, inlining nested Choice alts in parent Choice for save dispatch, and using `*:body=@` for opaque sub-language bodies (constraint blocks, SVA properties, covergroup items)

5. **[`EXAMPLES.md`](EXAMPLES.md)** — worked Python and CLI examples per capability.

## A starting prompt structure for grammar authoring

This is a working pattern, not gospel — refine for your agent's behaviour. The key beats:

```
You are writing a rawast grammar for the following format: <FORMAT NAME>.

Format specification (study it carefully):
<paste spec, BNF, or vendor reference>

Required reading (already in your context):
- rawast grammar DSL reference (docs/rawast-format.md)
- Engine architecture, lint anti-patterns (docs/ARCHITECTURE.md)
- Reference grammars (grammars/tcl.rawast, grammars/lefdef.rawast)

Your output is a single .rawast file. Conventions:

- Declare `use:` parser groups (likely `std` for text formats).
- Declare `start:` rule.
- Define every rule in the format's spec, top-down.
- Use `:type=` discriminators on Choice alternatives so save dispatch works.
- For optional sub-clauses, prefer first-token-distinct sibling rules over a
  nested Choice inside a wildcard rule (the wildcard-Choice-type-emit lint
  fires otherwise).
- Use `:name[]=@` for multi-instance clauses, not the catcher flatten.
- For vendor-extension or opaque body content, use `*:body=@, "STOP" newline`.
- **Pick the right literal form**: `"text"` is byte-prefix (matches "text"
  even as the prefix of "textfoo"); `'text'` is strict / word-bounded
  (only matches "text" when followed by a non-word char or EOF). Use
  single-quote `'...'` for language reserved words and spec keywords
  that mustn't consume into identifiers (e.g. `'not'`, `'END'`, `'MACRO'`).
  Use double-quote `"..."` for punctuation, openers, or intentional
  prefix captures (e.g. `"LEF58_":@`). See `docs/rawast-format.md` §4.2a.
- **Emit the application's IR shape directly via bindings — do NOT plan
  to post-process the AST in code.** The binding mechanism
  (`:field=@`, `:field[]=@`, `:field=const`) is how the grammar produces
  the shape the host wants. If the host wants `{op, args}`, the grammar
  should emit `{op, args}`. Reach for the binding mechanism early; writing
  a "lower" / "normalize" step in code is the anti-pattern. See the
  "Design the grammar to emit your application's IR" section below.
- **Parse structured lists DIRECTLY, don't capture-and-subparse.** For
  comma- or semicolon-separated content (enum labels, struct fields,
  modport ports, parameter lists, etc.), use the outer grammar's
  `repeat+ <ITEM>:items[]=@ separator ","` primitives — not
  `*:body=@:subparse="ITEMS"`. Subparse is the right tool when the
  inner content is a genuinely DIFFERENT sub-language (SVA temporal
  expressions, constraint distributions, embedded SQL); it's the
  wrong tool when the inner is just a list. See the "Practical tips"
  section below for the full heuristic.
- Add a comment with the spec section number for each non-trivial rule.

Before emitting, mentally walk the grammar through:
1. `rawast lint` — would any rule trigger LL(1) ambiguity, the nested-Choice
   anti-pattern, or a `*` without a following literal sibling?
2. Save direction — can each Choice alternative be uniquely identified by a
   discriminator field in the saved value tree?

Output ONLY the .rawast text. No commentary outside it.
```

The lint discipline is what separates a first-pass grammar that works from one that fails mysteriously at save time. An agent that knows the patterns up-front produces cleaner grammars in fewer iterations.

## Design the grammar to emit your application's IR, not a "raw" parse tree

A common anti-pattern when agents author rawast grammars: write the grammar to produce a "structural" parse tree (`{or: [a, b, c]}`, `{left, ops: [{op, rhs}, ...]}`, etc.) and then write a Python `lower()` function that walks that tree and reshapes it into the application's IR (`{op, args}`). This treats rawast as a tokenizer + tree builder, with semantics post-processed in code.

**This leaves real power unused.** The binding mechanism (`:name=@`, `:name[]=@`, `:name=const`) does semantic work. If you reach for it during grammar design, the grammar can emit your IR directly — no post-processing step.

### Concrete example

A constraint expression language with `or` / `and` / comparisons / arithmetic, where the application wants a uniform IR of `{op: str, args: [node, ...]}`.

**The shallow / wrong way:**

```
OR ignore whitespace: sequence dict {
  repeat+ <AND>:or[]=@ separator 'or'
}
```

Grammar emits `{or: [a, b, c]}`. Then `lower()` in Python walks this and produces `{op: "or", args: [a, b, c]}`. Two files, two surfaces to keep in sync.

**The grammar-does-the-work way:**

```
OR: choice {
  <OR_MULTI>,
  <AND>             // pass-through when only one operand
}

OR_MULTI: sequence dict {
  <AND>:args[]=@,
  'or':op="or",     // constant emit attached to the separator Key
  <AND>:args[]=@,
  repeat <OR_REST>
}

OR_REST: sequence { 'or', <AND>:args[]=@ }
```

Same engine, same parser invocation, but `parse_string("a or b or c")` directly returns:

```python
{"op": "or", "args": [{"ref": "a"}, {"ref": "b"}, {"ref": "c"}]}
```

No `lower()` function. The Choice between "multi" and "single pass-through" handles the single-operand case (where wrapping in `{op: "or", args: [a]}` would be wrong); the `'or':op="or"` Key emits the operation discriminator alongside matching the literal.

### The patterns

| Application-IR shape | Grammar pattern |
|---|---|
| `{op: "X", args: [a, b]}` (binary) | `<LHS>:args[]=@, "X":op="X", <RHS>:args[]=@` |
| `{op: "X", args: [arg]}` (unary) | `"X":op="X", <ARG>:args[]=@` |
| `{op: "X", args: [a, b, c, ...]}` (n-ary associative) | `choice` between a multi-form (≥2 args with `'X':op="X"` separator pattern) and pass-through of the single operand |
| `{type: "X", field1, field2, ...}` (typed struct) | `"X":type="X", <FIELD1>:field1=@, <FIELD2>:field2=@` |
| `{kind: "X", value}` (tagged variant) | `"X":kind="X", <VALUE>:value=@` |
| `{ref: "name"}` (leaf reference) | `qualified_identifier:ref=@` |
| `{num: 42}` (leaf scalar) | `int:num=@` (or `float:num=@`) |

### The honest limitation

PEG can't natively left-recurse. For genuinely left-associative chains (`a - b - c` must mean `(a - b) - c`, not `a - (b - c)` — different result for non-associative `-`), the grammar can't emit a left-folded `{op, args}` shape in one pass. Three options when you hit this:

- **Accept a `{left, ops: [...]}` shape for these specific operators.** The compiler walks them differently from the binary `{op, args}` cases. Document the shape; the compiler handles two AST shapes instead of one.
- **Emit parallel arrays** — `{args: [a, b, c, d], ops: ["+", "-", "+"]}`. The compiler folds left in a few lines; this is pattern-matching operator semantics, not "lowering" structural shape.
- **Require parentheses** — `a - b - c` becomes a syntax error; the user writes `(a - b) - c`. Grammar emits clean binary `{op, args}` everywhere; one user-facing constraint.

Most application languages can use the parenthesize-required approach for the rare cases. Don't write a full `lower()` function unless you've ruled out these three.

### Sanity check before adding any post-processor

If you find yourself writing code that walks the parsed AST and produces a different-shaped AST, ask: **what binding could the grammar have used to emit the right shape directly?** Most of the time the answer is "`:field=const` constant + `:args[]=@` list-append on the operand Refs" — and the grammar gets a few lines cleaner while losing an entire post-processing module.

The grammar IS the schema for the application's IR. Use it.

### When to inline a nested Choice in its parent (save dispatch)

A common shape: `OUTER_CHOICE` has alternatives, one of which is `<INNER_CHOICE>` which itself has alternatives. On the parse side this works fine — the outer Choice tries alts, picks one, the inner Choice runs and picks an alt. On the save side, the dispatcher hits the outer Choice, tries to dispatch on the value's `type` discriminator, finds `INNER_CHOICE` as an alt, recurses, and the inner Choice has to dispatch on the SAME `type`. Depending on how many levels of nesting and how distinct the discriminators are, the dispatch can fail to find a unique alt.

**Fix**: inline the inner Choice's alternatives directly into the outer Choice. Each alternative is a flat sibling of the outer Choice rather than a child of an intermediate Choice rule. Same parse behavior; save dispatch sees a single flat list of distinct alts.

Example from the SystemVerilog grammar — class declarations were originally:

```
DESCRIPTION: choice { ..., <CLASS_DECL>, ... }
CLASS_DECL:  choice { <CLASS_DECL_VIRTUAL>, <CLASS_DECL_PLAIN> }
CLASS_DECL_VIRTUAL: sequence dict { 'virtual', 'class':@:type="class", ... }
CLASS_DECL_PLAIN:   sequence dict {            'class':@:type="class", ... }
```

This worked for parse but broke save: both inner alternatives have the same `type="class"` discriminator, so the dispatcher couldn't tell them apart. The fix:

```
DESCRIPTION: choice { ..., <CLASS_DECL_VIRTUAL>, <CLASS_DECL_PLAIN>, ... }
CLASS_DECL_VIRTUAL: sequence dict { 'virtual', 'class':@:type="class", ... }
CLASS_DECL_PLAIN:   sequence dict {            'class':@:type="class", ... }
```

Both alternatives are now direct siblings of DESCRIPTION's Choice. The `'virtual'` Key on `CLASS_DECL_VIRTUAL` differentiates the parse-side match; on save, the dispatcher inspects the value for a `virtual` field that CLASS_DECL_VIRTUAL's const binding sets (the const `virtual=true`) and dispatches correctly. The intermediate `CLASS_DECL` Choice was removed.

**Heuristic**: if two alternatives of an outer Choice share a discriminator (e.g. `type="class"`), and they're disambiguated by a leading `?'KEYWORD'` Key or a const binding rather than a unique `type` value, inline them both as siblings of the outer Choice. The intermediate Choice level only helps if the alternatives have genuinely distinct discriminators that the dispatcher can route on.

### A common reluctance: "won't a `choice` with shared prefixes be O(2^n) slow?"

Agents are often hesitant to write `choice` patterns where two alternatives share a leading rule — like the OR / OR_MULTI pattern above, where both branches start with `<AND>`. The fear is "this will explode exponentially like the bad PEG examples."

**It won't.** rawast's PEG handles alt-failure by restoring the input cursor and trying the next alternative — *linear in the depth of the failed alt*, not exponential. For the OR_MULTI / AND pattern:

- Single operand `a`: tries OR_MULTI, parses `<AND>` once, fails on the required `'or'`, falls back to the bare `<AND>` alternative, parses `<AND>` once more. `<AND>` is parsed twice. That's the worst case.
- Two operands `a or b`: tries OR_MULTI, parses `<AND>` once, matches `'or'`, parses `<AND>` again. Success on the first attempt. `<AND>` parsed twice total — once per operand.
- Three operands: parsed three times total. Linear in operand count.

There is NO exponential blowup because the engine commits to the first successful alt and never re-evaluates higher-level alternatives after a child succeeds. The repeated `<AND>` work in the single-operand case is bounded — it's one extra parse of the child rule, not an arbitrary cascade.

If `<AND>` itself has heavy sub-rules that are expensive to re-parse, the bounded duplicate matters more. In that case factor common prefixes out into a leading sequence:

```
OR: sequence dict {
  <AND>:args[]=@,
  ?<OR_TAIL>
}
OR_TAIL: sequence dict {
  'or':op="or",
  repeat+ <AND>:args[]=@ separator 'or'
}
```

But this trade is rarely worth it — the linter (LL(k) lookahead) won't flag the shared-prefix Choice as ambiguous because the alternatives diverge at the next token (`'or'` vs end-of-input). The engine handles it cleanly; the alt-failure cost is small and bounded.

**Heuristic**: write the cleanest grammar shape first. If profiling shows a hot spot in an alt-failure path, refactor that specific Choice. Don't preemptively contort the grammar.

### The LL(k) lint warning on shared-prefix Choices is informational

The lint's LL(k) check flags Choices where two alternatives share a leading rule that can't be disambiguated by first-token analysis within bounded depth. The OR / OR_MULTI pattern above is a typical example — both branches start with `<AND>`, and the lint can't prove they diverge within its lookahead window because `<AND>` can begin with a generic Parse (identifier, number).

**This is informational, not an error.** The rawast engine restores input position on every Choice-frame alt failure — each alternative attempt is wrapped in input-cursor `mark()` / `reject()`, so partial alt failures cleanly restore position and the next alt is tried. The shared-prefix pattern parses correctly. The lint just can't see that statically, so it surfaces the pattern so you can decide whether it's intentional.

Two reasonable responses:

- **Restructure** the grammar so the alternatives diverge within LL(k) lookahead. Eliminates the alt-failure cost (which is small but real) and silences the warning. Often not possible if you need the direct `{op, args}` emission — the alt-failure pattern is what makes single-vs-multi-operand work cleanly.
- **Accept** the warning as a permanent design note in the lint output. The grammar produces correct output; the warning documents that the fall-through pattern is intentional. No Python loader hook, no special silencing flag — just `rawast lint` shows a few informational warnings on these specific Choices.

The lint does not provide a per-Choice silencer. There is no `// lint: ignore` comment, no `silence_lint: true` attribute. The lint output is honestly informational: take the design feedback (restructure to factor out the shared prefix if the cost matters), or accept the warning as a permanent design note documenting the intentional fall-through.

## How an agent uses the parsed AST

The AST is a normal Python dict (or JSON for language-agnostic agents). Walk it as you would any JSON:

```python
import rawast

g = rawast.Grammar.load("my_format.rawast")
ast = g.parse_file("input.fmt")

# Walk the structure — same patterns as JSON
for item in ast.get("items", []):
    if item.get("type") == "Foo":
        # ... agent logic here ...
        process_foo(item["name"], item["value"])
```

For agents using Pydantic models for type-safe extraction:

```sh
rawast pydantic my_format.rawast > my_format_models.py
```

```python
import rawast, my_format_models as M

g = rawast.Grammar.load("my_format.rawast")
ast = g.parse_file("input.fmt")

typed = M.Root.model_validate(ast)
# Now typed.items[0].type, typed.items[0].name etc. — IDE autocomplete,
# Pydantic validation, refusal of fields the grammar can't save back.
```

## How an agent constructs files (bidirectional save)

The Pydantic model shape matches the grammar's save shape exactly. The agent builds typed, the engine emits bytes:

```python
import rawast, my_format_models as M

obj = M.Root(
    items=[
        M.Foo(type="Foo", name="thing", value=42),
        M.Bar(type="Bar", flag=True, label="other"),
    ],
)

g = rawast.Grammar.load("my_format.rawast")
text = g.save(obj.model_dump(exclude_none=True, by_alias=True))
with open("output.fmt", "wb") as f:
    f.write(text)
```

Round-trip property: `parse → typed model → edit → dump → save` is lossless. An agent reading a config file can modify one field and save the rest verbatim (whitespace, comment positions, and clause ordering may canonicalize — the *structure* is preserved).

## Parsing arbitrary character sequences with pure grammar

When the input contains a token that no shipped terminal parser handles — a hex-digit run with `x`/`z`/`?` characters, a Verilog-style underscored number, a custom literal form — the first instinct is to write a C++ terminal parser. **Try pure grammar first.** Most "needs a custom parser" cases turn out to be expressible with two existing mechanisms in combination.

### The technique

Two pieces:

1. **`RULE ignore:` (empty list)** disables whitespace skipping inside the rule. Adjacent characters in the rule body must match without intervening whitespace — exactly the behaviour you need to lex a token as one tight sequence.

2. **Character-level `choice`** of single-char strict keys (`"0":@`, `"1":@`, …, `"F":@`) acts as a character class. Combined with `repeat+` you get a digit run.

### Concrete example — Verilog based-number digit run

The Verilog source form `8'h1_0_FF_xz?` has a digit run containing hex digits, `x`/`z`/`?` placeholders, and underscores as visual separators. No std parser handles this. With pure grammar:

```rawast
BASED_DIGITS ignore: sequence array {
  repeat+ <BASED_DIGIT>
}

BASED_DIGIT: choice {
  "0":@, "1":@, "2":@, "3":@, "4":@, "5":@, "6":@, "7":@,
  "8":@, "9":@,
  "a":@, "b":@, "c":@, "d":@, "e":@, "f":@,
  "A":@, "B":@, "C":@, "D":@, "E":@, "F":@,
  "x":@, "X":@, "z":@, "Z":@, "?":@,
  "_":@
}
```

That's it. `BASED_DIGITS` returns an array of single characters; the host joins or treats as a list. The `ignore:` keeps the digits packed; the Choice is the character class.

### When this works well

* **Lexical tokens with a fixed character alphabet.** Hex strings, binary strings, alphanumeric ID continuations, any "consume run of chars in set X" pattern.
* **Format-specific separators** like `_` in numbers, `'` between size and base, or `.` in dotted identifiers — express as literal Keys in a sequence.
* **Composing with std parsers in a Choice.** Most numbers in most grammars need only `std.int` + `std.float` + this technique for the format-specific portion (like a Verilog `'hFF` suffix). Don't write a parser that does everything; let `choice { <YOUR_SPECIAL>, std.float, std.int }` dispatch.

### When a custom Parse terminal IS the right answer

* **"Consume until X" semantics.** Verilog's escaped identifier `\<chars until whitespace>` consumes any printable char. Expressing 95+ char alternatives in a Choice is impractical; a 10-line custom parser is cleaner.
* **Structural semantics inside the token.** If you need to compute a value (parse digits into an int with overflow check, decode an escape sequence) rather than just capture chars, do it in C++.
* **The token is a single canonical form across many grammars.** `std.int`, `std.float`, `std.identifier`, `std.string` exist for this reason — write once, reuse everywhere.

### Tips and caveats

* **Order matters in the outer Choice.** A `BASED_NUM` rule must come before plain `INTEGER_NUM` in a Choice, because `8'hFF` shares its leading `8` with the integer form. PEG's alt-failure recovery picks the right one but only if you list the more-specific match first.
* **The lint flags some Choice patterns as informational.** A long character-set Choice (`"0":@, "1":@, …`) has many alternatives with disjoint first tokens, so it parses correctly and is lint-clean. Don't worry about it.
* **Strict keys preserve case sensitivity.** `"A":@` matches only `A`, not `a`. For case-insensitive matching, add both alternatives explicitly. (Verilog accepts `8'hff` and `8'hFF`; the Choice lists both letter cases.)

This technique covers most "the grammar can't express my token" cases. Reach for it before writing a custom Parse terminal — fewer lines, no recompile, the entire spec lives in the grammar source.

## Practical tips for agents

**Always lint after generating a grammar.** Either via the CLI (`rawast lint <grammar>`) or programmatically (`Grammar.load(...).lint()`). The lint catches LL(1) ambiguity, the wildcard-Choice-type-emit anti-pattern, and `*` raw-consume misuse — three failure modes that are hard to diagnose at parse time but trivial to fix at grammar-design time. Feed lint output back into your agent as a follow-up turn so it can iterate.

**Use `subparse` for embedded sub-languages.** If a value is itself a structured form (Tcl's `if { ... }` block, an embedded SQL fragment, a templating expression), capture the body as a literal string with the appropriate terminal parser, then call `Grammar.parse_string(body, start="SUBRULE")` from your agent code to recurse. Same engine, recursive call — that's how Tcl's runtime evaluates braced bodies, and rawast lets your agent do the same trick for any embedded language.

Subparse is **bidirectional** — the engine round-trips the structured sub-tree back to text on save by serializing through the subparse rule first, then writing the result through the underlying parser. The AST stores the typed sub-tree; save reconstructs the bytes. Author the subparse rule as if it were a normal grammar; both directions Just Work.

**Don't over-use subparse for plain structured lists.** A common over-engineering pattern: capture content between `{ ... }` as raw text, then add `:subparse="ITEMS"` to re-parse it into a list. If the content is just a comma- or semicolon-separated sequence (enum labels, struct fields, modport ports, parameter list, etc.), parse it DIRECTLY in the outer rule with `repeat+ <ITEM>:items[]=@ separator ","` — no capture, no re-entry, no subparse rule. The grammar engine handles structure inline and save round-trips naturally.

Subparse is the right tool when the inner content is a genuinely DIFFERENT sub-language — different ignore policy, different operators, different lexical structure (SVA temporal expressions, constraint distributions, embedded SQL, regular-expression literals). It's the wrong tool when the inner is just a list with familiar separators. **Heuristic**: if you can write the rule body with `repeat`/`separator`/`?<X>`/`<EXPR>` primitives the outer grammar already uses, parse it directly. If the inner needs custom tokens or a different ignore policy, capture + subparse.

**Use rule-local `ignore` for context shifts.** If part of your format treats whitespace differently from the rest (e.g. a quoted string interior where whitespace is literal data), declare a `RULE ignore:` override on that rule's body. The engine pushes the override on rule entry and pops on exit; the rest of the grammar is unchanged.

**Test grammars with `rawast pycode` against real files.** Once a grammar exists, point `rawast pycode` at a real instance of the format to generate Python source that reconstructs the same model. If the round-trip diverges, the grammar has a coverage gap. This is faster than hand-writing test fixtures and catches gaps the agent missed at authoring time.

**Track grammars in git like code.** Version them alongside the format spec they model. A grammar at v1.0 should produce the same AST for the same file across runs. Treat grammar updates the same way you treat schema migrations.

**Default keywords to strict (`'token'`).** When the format spec defines a reserved-word vocabulary that mustn't collide with identifiers — language keywords, section names, named clauses — author them as `'KEYWORD'` (single-quote, word-bounded) rather than `"KEYWORD"` (byte-prefix). Catches a whole class of bugs at parse time: byte-prefix `"not"` silently matches the prefix of `"notch"`, leaving `"ch"` as a phantom identifier; strict `'not'` correctly rejects, so a Choice over `'not'` / `notch` dispatches to the right branch without depending on hand-ordering. The cost of strict is one peek per match; the benefit is a class of bugs that doesn't reach runtime.

**Reach for hierarchy, not flat "always wrap." When you write a precedence ladder (or any layered grammar), don't make every level emit a wrapper unconditionally.** The tempting pattern looks like:

```
LOR:  sequence dict { <LAND>:lhs=@, repeat <LOR_TAIL>:tail[]=@ }
LAND: sequence dict { <BOR>:lhs=@,  repeat <LAND_TAIL>:tail[]=@ }
BOR:  sequence dict { <BXOR>:lhs=@, repeat <BOR_TAIL>:tail[]=@ }
...
```

Each level emits `{lhs, tail}` whether or not its operator fired. A bare identifier ends up with 10 layers of `{lhs:{lhs:{lhs:...}}}` wrappers piled around it from every precedence level above. That noise is real cost: every consumer downstream (Python tests, codegen, save dispatch, semantic analyzers) has to walk through those empty wrappers, and the JSON dump for `defined(X)` becomes ~80 lines of dict-of-dict before reaching the actual primary.

Use `choice { CHAIN, NEXT }` instead. CHAIN matches only when there's ≥1 operator (via `repeat+`); otherwise the choice falls through to NEXT cleanly:

```
LOR:       choice { <LOR_CHAIN>, <LAND> }
LOR_CHAIN: sequence dict { <LAND>:lhs=@, repeat+ <LOR_TAIL>:tail[]=@ }
```

Now a bare identifier is emitted as `{type:"ident", name:"X"}` with no wrappers. `a && b` is one chain node at the AND level — every other level passes through. The AST shape mirrors the actual operator structure of the expression instead of the structure of the grammar's precedence ladder.

The general principle: **emit structure that reflects what's in the source, not what's in the grammar.** Always-wrap is tempting because it's mechanical to write — every level looks the same. But the savings at authoring time become a tax forever after, paid by every consumer of the AST. Hierarchical parsing close to the language's actual structure makes semantics extraction easier downstream, makes the AST diffable / serializable as something a human can read, and avoids embedding the grammar's implementation detail into the data shape. When you find yourself writing a sequence of identical-shaped rules at each level of a ladder, stop and ask whether the choice+`repeat+` form would give you a more honest tree.

This applies beyond expression ladders — any time you're modeling layers of optional structure (modifier chains, port qualifiers, declaration prefixes), prefer the "chain if present, otherwise skip the wrapper" form.

**Save-side caveat (current implementation).** The save-side dispatcher commits to the first Choice alternative whose surface shape matches the value, with no lookahead through the rest of the rule. The `choice { CHAIN, NEXT }` form works for parse but trips the save dispatcher when CHAIN and NEXT both produce shapes that look like `{lhs, tail}` at the same precedence in NEXT's body — the dispatcher has no way to distinguish a `LOR_CHAIN`-shaped value from an `LAND_CHAIN`-shaped one without inspecting the `op` strings in `tail`, which it doesn't currently do. The pattern is fine for grammars whose CHAIN and NEXT shapes are structurally distinct (e.g., `PP_EXPR` where the leaves are typed primaries like `{type:"defined",...}` and `{type:"int",...}`), but the SV expression ladder hits this issue because every level can emit the same `{lhs, tail}` shape. Until the save dispatcher learns to peek at discriminator fields inside tail entries, the SV-style ladder stays on the always-wrap form. Greenfield grammars should still reach for the choice form first and only fall back to always-wrap if save round-trip is required AND the levels share output shapes.

**Diagnose grammar gaps with `RAWAST_TRACE`, not bisection.** When a parse fails with `unexpected content after start rule completed (byte N, line L, column C)`, the engine has told you *where* it stopped but not *why*. Resist the urge to manually shrink the input until you find the breaking construct — set `RAWAST_TRACE=1` in the environment and re-run the same parse. The trace dumps every frame the engine pushed, every alternative tried, and the exact failure message for each one, indented by stack depth. Read from the bottom (the deepest failures) upward to see which rule got the furthest before bailing.

A typical session for diagnosing one failing file:

```sh
RAWAST_TRACE=1 python -c "
import rawast
g = rawast.Grammar.load('grammars/systemverilog.rawast')
g.parse_file('the_failing_file.sv')
" 2>&1 | tail -80
```

The last 30–80 lines usually contain enough context: which named rule was active, which alternative of which choice failed, and the literal/parser-name the engine was trying to match. Once you see `FAIL: expected literal 'unsigned'` inside `unwind PARAM_DECL_USERTYPE` you know the gap: `PARAM_DECL_USERTYPE` doesn't accept the `int unsigned` form.

Tracing scales from one file to dozens — pipe through `grep -E 'unwind|FAIL'` to skim only the failures across a large run. Zero runtime cost when the env var is unset (one bool check at each instrumentation site).

## When NOT to use rawast for agent work

- **The format is unstructured.** Free prose with no scope hierarchy is a job for the LLM directly, not a parser.
- **One-off use.** If the agent will see ≤50 instances of the format, writing the grammar takes longer than letting the LLM handle each instance. The threshold scales with format complexity.
- **You need lossy summarization.** rawast's strength is faithful round-trip. If the agent's job is to extract a 3-sentence summary from a 10-page document, the LLM directly is the right tool.

## See also

- [`rawast-format.md`](rawast-format.md) — grammar DSL specification (required reading for agents authoring grammars)
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — engine internals, lint anti-patterns
- [`GRAMMARS.md`](GRAMMARS.md) — shipped grammars as reference examples
- [`EXAMPLES.md`](EXAMPLES.md) — worked code examples
- [`FEATURES.md`](FEATURES.md) — engine capability list
- [`CLI.md`](CLI.md) — `rawast lint`, `rawast pydantic`, `rawast pycode` reference
