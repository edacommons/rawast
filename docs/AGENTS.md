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
- Add a comment with the spec section number for each non-trivial rule.

Before emitting, mentally walk the grammar through:
1. `rawast lint` — would any rule trigger LL(1) ambiguity, the nested-Choice
   anti-pattern, or a `*` without a following literal sibling?
2. Save direction — can each Choice alternative be uniquely identified by a
   discriminator field in the saved value tree?

Output ONLY the .rawast text. No commentary outside it.
```

The lint discipline is what separates a first-pass grammar that works from one that fails mysteriously at save time. An agent that knows the patterns up-front produces cleaner grammars in fewer iterations.

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

## Practical tips for agents

**Always lint after generating a grammar.** Either via the CLI (`rawast lint <grammar>`) or programmatically (`Grammar.load(...).lint()`). The lint catches LL(1) ambiguity, the wildcard-Choice-type-emit anti-pattern, and `*` raw-consume misuse — three failure modes that are hard to diagnose at parse time but trivial to fix at grammar-design time. Feed lint output back into your agent as a follow-up turn so it can iterate.

**Use `subparse` for embedded sub-languages.** If a value is itself a structured form (Tcl's `if { ... }` block, an embedded SQL fragment, a templating expression), capture the body as a literal string with the appropriate terminal parser, then call `Grammar.parse_string(body, start="SUBRULE")` from your agent code to recurse. Same engine, recursive call — that's how Tcl's runtime evaluates braced bodies, and rawast lets your agent do the same trick for any embedded language.

**Use rule-local `ignore` for context shifts.** If part of your format treats whitespace differently from the rest (e.g. a quoted string interior where whitespace is literal data), declare a `RULE ignore:` override on that rule's body. The engine pushes the override on rule entry and pops on exit; the rest of the grammar is unchanged.

**Test grammars with `rawast pycode` against real files.** Once a grammar exists, point `rawast pycode` at a real instance of the format to generate Python source that reconstructs the same model. If the round-trip diverges, the grammar has a coverage gap. This is faster than hand-writing test fixtures and catches gaps the agent missed at authoring time.

**Track grammars in git like code.** Version them alongside the format spec they model. A grammar at v1.0 should produce the same AST for the same file across runs. Treat grammar updates the same way you treat schema migrations.

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
