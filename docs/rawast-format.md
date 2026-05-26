# The `.rawast` grammar format

Status: **Draft / pre-implementation.** This document specifies the source-text
format that rawast grammars are written in. The engine bootstraps via JSON
grammar files; `.rawast` is the canonical hand-authoring format. The two
serialisations describe the same in-memory grammar data; swapping between
them is a loader-level concern that does not affect the engine, the `.jast`
container, or any downstream consumer of parsed data.

## 1. Overview

A `.rawast` file is a sequence of **rule definitions**, each defining one
named non-terminal of a grammar. The definitions are loaded together into a
`Grammar` object — the same in-memory data model produced by loading the
equivalent JSON grammar file. `.rawast` and `.json` grammar files are two
serialisations of the *same data*; either can produce any grammar the engine
supports.

A small example:

```
start: <VALUE>

VALUE: choice {
  <STRUCT>,
  <LIST>,
  string,
  int,
  float,
  "null":null,
  "true":true,
  "false":false
}

LIST: sequence array {
  "[",
  repeat <VALUE> separator ",",
  "]"
}

PAIR: sequence {
  string:@=,
  ":",
  <VALUE>
}

STRUCT: sequence dict {
  "{",
  repeat <PAIR> separator ",",
  "}"
}
```

That defines a complete JSON grammar in 22 lines. The JSON-form equivalent
is roughly 50 lines and considerably less readable.

## 2. Lexical structure

### 2.1 File encoding

UTF-8. The engine treats input as a byte stream; non-ASCII bytes inside
quoted literals and identifiers are preserved verbatim.

### 2.2 Whitespace and line terminators

Whitespace (space, tab, carriage return, newline) is allowed between any
two tokens and is consumed by the engine's ignore-list mechanism. There
is no significant whitespace; indentation has no meaning.

### 2.3 Comments

Two comment styles are supported, both placed in the ignore list:

```
// line comment to end of line
/* block comment, possibly
   spanning multiple lines */
```

### 2.4 Identifiers

```
identifier ::= [a-zA-Z_][a-zA-Z0-9_]*
```

Identifiers are case-sensitive. The convention used throughout this
document and the bundled grammars:

- **UPPER_CASE** identifiers name grammar **rules** (e.g. `VALUE`, `STRUCT`).
- **lower_case** identifiers name **terminal parsers** registered on the
  grammar (e.g. `int`, `float`, `string`, `identifier`).

The convention is not enforced by the engine — rule and parser names live
in separate registries — but mixing them within a file is discouraged.

### 2.5 String literals

Double-quoted, with backslash-pass-through for embedded quotes:

```
"hello"
"with \"embedded\" quotes"
"//\\/* etc */"
```

The backslash itself is preserved verbatim (this is the engine's existing
pass-through escape behaviour from `DoubleQuoteStringParser`; higher-level
escape interpretation is a future concern).

### 2.6 Reserved words

The following identifiers have grammatical meaning and may not be used
as parser names or as bare-identifier expressions:

```
sequence  choice  repeat  separator  array  dict  null  true  false
indent    tab     space   newline    tail   use
```

The first row are the structural keywords (§4.6–4.8 and §4.3
constants). The second row are the **save-side pretty-print postfix
attributes** (§4.6).

A future revision may add `optional` and other keywords.

### 2.7 The `@` and `@=` markers

`@` is a placeholder meaning **"the value at this position."** It is used
in binding suffixes to flow the result of an expression into the
surrounding catcher.

`@=` is the **dict-key binding** marker: emit the value as the next
dict key (equivalent to `is_name=true` on the producing node).

Both are explained fully in §4.

## 2.8 The `use:` directive

A `.rawast` file may declare which **terminal-parser groups** it needs
by listing them at the top of the file:

```rawast
use: gdsii
use: standard

start: <LIBRARY>
LIBRARY: sequence dict { ... }
```

Multiple groups can be combined comma-separated:

```rawast
use: gdsii, standard
```

When the loader encounters a `use:` directive it looks up each named
group in a process-wide registry of parser-group factories and applies
each to the target Grammar before processing any rule definitions. If
a referenced group has not been registered, the load fails with a clear
diagnostic ("`use: parser group 'X' not registered`") — much friendlier
than the cryptic parse-time failure that would otherwise occur when an
unknown parser is referenced.

Built-in groups (compile-time registered in `librawast`):

| Group name | Provides |
| :--- | :--- |
| `std` | Common terminal parsers — `identifier`, `qualified_identifier`, `int`, `uint`, `float`, `string`, `whitespace`, `line_comment`, `block_comment` |
| `gdsii` | All 47 GDSII binary record parsers (`header`, `bgnlib`, …) — referenced bare or group-qualified (`gdsii.header`) |
| `lefdef` | LEF/DEF-specific `identifier` (accepts hyphens, slashes per real-world naming) and `line_comment` (`#` to EOL) |
| `tcl` | Tcl terminals modelled on Dodekalogue rules — `hspace`, `newline`, `comment`, `brace_group`, `quoted_string`, `bracket_sub`, `bare_word`, `expand_marker`, `var_name`, `until_paren`, `escape`, `literal_run` |

Additional groups can be registered from host C++ code via
`rawast::register_parser_group("name", register_fn)`; see
`include/rawast/parsers_registry.hpp`. Runtime plugins loading
.so/.dll files at startup is a potential M5+ extension; the static
registry is sufficient for M1–M4 scope.

## 3. Rule definitions

A `.rawast` file is a sequence of rule definitions:

```
rule_definition ::= identifier ':' expression
```

Rule definitions are separated by whitespace only — no explicit
terminator. Each definition registers its identifier as a named rule.

The special pseudo-rule `start` designates the top-level entry point of
the grammar:

```
start: <VALUE>
```

`start` must be a reference (`<NAME>`) to another rule.

### 3.1 Rule-local ignore — `RULE ignore PARSER1 PARSER2 …: <body>`

A rule definition may carry an **ignore-list attribute** between the
rule name and the colon. The named parsers become the active ignore
set whenever the parse driver is inside that rule (or any rule it
calls that doesn't carry its own override):

```rawast
// Grammar-level "default ignore" — attach it to the start rule.
start: <SCRIPT>
SCRIPT ignore tcl.hspace: sequence dict { ... }

// Override on a different rule — its sub-tree uses a different
// ignore set. Useful when one part of the grammar needs to treat
// newlines as significant (here: command separators) and another
// part needs them ignored (here: expression context).
EXPR ignore tcl.hspace tcl.newline: sequence dict { ... }

// Explicit empty list — override to "ignore nothing", useful for
// token-internal contexts where whitespace is part of the data.
WORD_SEGMENTS ignore: sequence dict { ... }
```

The list is **space-separated, not comma-separated**, and is
terminated by the `:`. Empty list (`RULE ignore: …`) explicitly
overrides to "ignore nothing"; rules without the attribute inherit
their caller's active ignore.

Inheritance semantics: the engine maintains an ignore-stack. On
entering a rule with an explicit override, the override is pushed;
on exit it is popped, restoring whatever the caller had active. A
rule with no `ignore` attribute simply uses the top of the stack.
This composes naturally — a SCRIPT-context rule that calls a sub-
rule sees the sub-rule run under SCRIPT's ignore, unless the sub-
rule overrides.

The standalone top-level `ignore: parser, parser, …` directive
(from earlier proposal drafts) is **gone** — attaching the ignore
to the start rule is the single canonical way to express grammar-
level default ignore. Existing grammars (json, gdsii, lef, def)
were migrated to the new form in lockstep with this change.

Combined with the `:subparse="<RULE>"` binding (§4.5), the
ignore-stack lets a single grammar express multiple sub-languages
each with their own whitespace policy. The Tcl grammar uses this
pattern: a script context (`tcl.hspace` ignored, newlines
structural), an expression context (`tcl.hspace` + `tcl.newline`
both ignored), and a word-internals context (nothing ignored)
all in one file.

## 4. Expressions

```
expression ::=  reference
             |  literal
             |  literal_with_constant
             |  parser
             |  binding
             |  sequence_expr
             |  choice_expr
             |  repeat_expr
             |  optional_expr
```

### 4.1 Reference — `<NAME>`

```
reference ::= '<' identifier '>'
```

A reference to another rule defined in the same file. References are
resolved lazily at grammar-load time, so forward references are allowed:

```
A: <B>
B: "ok"
```

### 4.2 Literal — `"text"`

```
literal ::= string
```

A literal token matched byte-for-byte from the input. Produces a Key
node. By default, literals are *structural* — they consume the input
text but emit no value to the surrounding catcher.

```
"[", "{", ":"
```

### 4.3 Literal with constant — `"text":CONSTANT`

```
literal_with_constant ::= string ':' constant
constant              ::= 'null' | 'true' | 'false' | '@' | other_literal
```

A literal whose successful match emits a constant into the surrounding
catcher. This is the discriminator pattern: the literal text in the input
selects a branch, and the constant identifies which branch was taken.

```
"null":null      // matches "null", emits the null singleton
"true":true      // matches "true", emits the true singleton
"false":false    // matches "false", emits the false singleton
"sequence":@     // matches "sequence", emits the string "sequence" itself
```

The `@` form is sugar for "emit the matched literal as a string value." It
saves repeating the literal:

```
"sequence":@       // equivalent to  "sequence":"sequence"
```

### 4.4 Parser — `identifier` or `group.identifier`

A bare identifier (not in reserved words, not enclosed in `<...>`) names
a terminal parser registered on the grammar. The dotted form
`group.identifier` is the same parser referenced via its group-qualified
alias — equivalent in every respect to the bare form:

```
int                 // SignedIntParser (from `use: std`)
uint                // UIntParser
float               // FloatParser
string              // DoubleQuoteStringParser
identifier          // IdentifierParser
qualified_identifier// QualifiedIdentifierParser (ident('.' ident)*)
gdsii.header        // GdsiiHeaderParser — same as `header` once `use: gdsii`
```

The set of available parsers is determined by what's been registered
when the grammar is loaded — primarily by `use:` directives at the top
of the file. Each group member is registered under both its bare
`local_name` and the `group.local_name` alias, so either form resolves
to the same parser.

### 4.5 Binding — `expression:bind_target`

```
binding      ::= expression ':' bind_target
bind_target  ::= '@'                              -- pass value through (default)
              |  '@='                             -- emit value as dict key
              |  identifier '=' bind_rhs          -- emit (identifier, rhs) pair into dict
bind_rhs     ::= '@'                              -- parsed expression's value
              |  literal                          -- constant value
literal      ::= string | int | float | 'true' | 'false' | 'null'
```

The binding suffix wraps an expression and controls how its produced
value is routed into the surrounding catcher.

**`X:@`** is the no-op binding — `X` produces a value, the value flows
into the catcher unchanged. Rarely needed in practice; the `:@` can be
omitted entirely.

**`X:@=`** flags the value as a **dict-key name** (equivalent to setting
`is_name = true` on `X`). The next non-name value emitted will be paired
with this name in the surrounding dict catcher.

```
PAIR: sequence {
  string:@=,     // the parsed string becomes the dict key
  ":",
  <VALUE>        // the next value becomes the corresponding dict value
}
```

**`X:name=@`** emits the pair (constant string `name`, value-of-X) into
the surrounding dict catcher. Useful for fixed field names:

```
NREPEAT: sequence dict {
  choice {"sequence":@, "choice":@}:type=@,        // type field
  ?choice {"array":@, "dict":@}:container=@,       // optional container field
  <ITEMS>:items=@                                  // items field
}
```

**`X:name=<literal>`** emits the pair (constant string `name`,
*literal-value*) into the surrounding dict catcher, *discarding* X's
parsed value. The literal can be a string, int, float, `true`, `false`,
or `null`. The expression X is still parsed (its side effect — typically
matching a discriminator token — is what makes the surrounding rule
fire); only its produced value is discarded.

This is the **discriminator-with-constant-emit idiom**, the natural
pattern for binary formats whose record types are zero-payload markers
that label the surrounding element. Example from GDSII:

```
BOUNDARY: sequence dict {
  gdsii.boundary:type="boundary",         // match the discriminator; emit ("type","boundary")
  ?gdsii.elflags:flags=@,
  ?gdsii.plex:plex=@,
  gdsii.layer:layer=@,
  gdsii.datatype:datatype=@,
  gdsii.xy:xy=@
}
```

Parser references support two surface forms — bare (`boundary`) and
group-qualified (`gdsii.boundary`). Both resolve to the same parser:
the registry registers each group member under its bare `local_name`
and under `group.local_name`. The dotted form is parsed as a single
token by `std.qualified_identifier`, so the AST shape stays flat
(`{type: "gdsii.boundary"}`).

In the JSON-grammar-format equivalent, the binding desugars to a
wrapper dict carrying `"type": "binding"` (for `name=@`) or
`"type": "binding_const"` (for `name=<literal>`); the loader expands
either into the appropriate Value-kind children. See §8.

**`X:subparse="<RULE>"`** is a special binding key recognised by the
engine. After X (a Parse-terminal returning a StringValue) succeeds,
the engine re-invokes the parse loop on the captured string with the
named rule as the new start. The resulting sub-tree replaces X's
string value. Same grammar, same engine, different entry point —
composes languages-within-languages without splitting them into
separate grammar files:

```
// Inside a hypothetical Tcl-script context:
IF_CMD: sequence dict {
  "if":type="if",
  tcl.brace_group:cond=@:subparse="EXPR",    // brace content re-parsed
  tcl.brace_group:body=@:subparse="SCRIPT"   //   through EXPR / SCRIPT
}
```

The engine resolves the subparse target's rule name to a NodeId at
grammar-load time; a missing rule fails the load with a clear error.
Subparse triggers create a fresh ignore-stack so per-rule ignore
overrides in the inner context don't leak into the outer parse.

### 4.5b Pretty-print attributes — postfix flags on items

Each item in an `items` list may carry **zero or more pretty-print
postfix attributes** after its expression (and after any binding). They
are pure save-side metadata: the parse direction ignores them entirely.

```
postfix_attr  ::= 'indent'              -- depth+1 for this Node's scope
              |   'tab'                 -- emit depth × indent_step before content
              |   'space'               -- emit ' ' after content
              |   'newline'             -- emit '\n' after content
              |   'tail' '=' string     -- emit string after content (escape-interpreted)
```

Attributes are space-separated and may appear in any order. The save
direction applies them in a fixed order, regardless of source order:

```
[ tab → depth × indent_step ] [ content ] [ tail ] [ space ] [ newline ]
```

**`indent`** bumps the save-time depth counter for the entire scope of
the Node it sits on. Nested `indent` flags accumulate. The bump happens
*before* `tab` fires, so an item with both `indent` and `tab` emits the
indent at the new (bumped) depth — convenient for the common
"indented-line" pattern.

**`tab`** emits the current depth's indentation (`depth × indent_step`,
where `indent_step` is a Grammar-level setting, default two spaces). It
fires at the beginning of the Node's content. Place `tab` only where
indentation should actually appear in the output — typically at the
start of lines.

**`space`** and **`newline`** emit `" "` and `"\n"` respectively after
the content. They are the common-case sugars; for anything else (`;\n`,
`\\\n`, `; `, custom separators) use `tail`.

**`tail="..."`** emits the string after content, with C-style escape
sequences interpreted at load: `\n`, `\t`, `\r`, `\\`, `\"`, `\0`.
Convention: use the bare `newline` flag for plain newlines; reserve
`tail` for non-newline strings or for combinations like backslash-
newline (`tail="\\" newline`).

Examples:

```
"{" newline,                          // "{" then "\n"
":" space,                            // ":" then " "
"PIN" space,                          // "PIN" then " "
",;" tail="; ",                       // (illegal — combining attrs needs space/newline keywords)
"$\\" newline,                        // emit `$\` then "\n" — but use tail="\\" newline
identifier tail=";" newline,          // identifier then "; then "\n"
<PAIR> tab indent,                    // emit indent before PAIR; depth+1 inside
repeat <PAIR> tab indent separator ",",  // each iteration indented at depth+1
```

The classic JSON-pretty pattern uses `indent` on the iterated item and
puts a trailing empty-key + newline before the closing brace:

```
STRUCT: sequence dict {
  "{" newline,
  repeat <PAIR> tab indent separator "," newline,
  "" newline,        // trailing newline before "}"
  "}" tab            // close brace at outer depth (auto-indented)
}
```

In the JSON-grammar-format equivalent, each pretty-print attribute is a
field on the item dict: `{"type": "key", "key": "{", "newline": true}`,
`{"type": "key", "key": "}", "tab": true}`, etc. The engine's `Node`
data type stores all five attributes uniformly; the two surface
serialisations are interchangeable.

**`indent_step` is a Grammar-level setting** (default `"  "`). Switch to
tabs via `Grammar::set_indent_step("\t")` in C++ before parsing.

**Runtime compact/pretty toggle.** `Grammar::save(out, value, pretty)`
takes a final `pretty` parameter (default `true`). When set to `false`,
the save direction skips `tab`, `indent` (no depth bump), and `newline`
emissions but still emits `space` and `tail` (since the grammar author
may rely on them for round-trip parseability — e.g. a space between two
adjacent identifiers). One grammar covers both pretty and compact
output; no separate "compact grammar" is required.

### 4.6 Sequence — `sequence [container] { items }`

```
sequence_expr ::= 'sequence' container? '{' items '}'
container     ::= 'array' | 'dict'
items         ::= expression (',' expression)*
```

A sequence of sub-expressions matched in order. The optional `container`
keyword annotates the surrounding catcher behaviour:

- *no container*: child values flow through unchanged to the parent's
  catcher (transparent grouping).
- `array`: at end of frame, accumulated values are materialised into an
  `ArrayValue`.
- `dict`: at end of frame, alternating (name, value) pairs are
  materialised into a `DictValue`.

Items inside the braces are separated by commas. Trailing commas are
permitted.

### 4.7 Choice — `choice { items }`

```
choice_expr ::= 'choice' '{' items '}'
```

Ordered alternation: the first alternative whose initial terminal
accepts is selected (predictive PEG; rawast does not backtrack at the
structural level). All alternatives must be terminal-prefix-
distinguishable; the grammar linter (planned) flags violations.

### 4.8 Repeat — `repeat expression [separator expression]`

```
repeat_expr ::= 'repeat' expression ('separator' expression)?
```

Zero-or-more iteration of the given expression, optionally separated
between iterations by the separator expression. Produces no container
of its own; the surrounding sequence's container catches the iteration
results.

```
repeat <VALUE> separator ","            // for arrays
repeat <PAIR> separator ","             // for dicts
```

### 4.9 Optional — `?expression`

```
optional_expr ::= '?' expression
```

Zero-or-one match of the given expression. On miss, the expression
contributes nothing to the parent's catcher.

```
?<MODIFIER>                             // optional modifier
?choice {"array":@, "dict":@}:container=@   // optional field
```

## 5. Containers and the catcher mechanism

At parse time, each frame on the engine's parse stack maintains a
`_values` list of emitted values streaming up from its children. Frames
whose grammar node has `container=Array` or `container=Dict` materialise
their accumulated values into an `ArrayValue` or `DictValue` at end-of-frame.

For dict containers, accumulated values must alternate `name` (with
`is_name=true`) and value entries. The order in which children are
written in the `.rawast` source must produce names and values in the
correct alternating order.

For dict containers used to build grammar-format trees (like NREPEAT
above), the `name=@` and `@=` binding forms make it straightforward to
emit explicit (name, value) pairs.

## 6. Examples

### 6.1 The JSON grammar in `.rawast`

```
start: <VALUE>

VALUE: choice {
  <STRUCT>,
  <LIST>,
  string,
  float,
  int,
  "null":null,
  "true":true,
  "false":false
}

LIST: sequence array {
  "[", repeat <VALUE> separator ",", "]"
}

PAIR: sequence {
  string:@=,
  ":",
  <VALUE>
}

STRUCT: sequence dict {
  "{", repeat <PAIR> separator ",", "}"
}
```

### 6.2 A CSV grammar in `.rawast`

```
start: <FILE>

FILE: sequence array {
  repeat <ROW> separator "\n"
}

ROW: sequence array {
  repeat <FIELD> separator ","
}

FIELD: choice {
  <QUOTED_FIELD>,
  unquoted_field
}

QUOTED_FIELD: string

// unquoted_field is a custom terminal parser, registered separately.
```

### 6.3 A `.rawast`-format grammar describing `.rawast` itself

A self-hosting fragment from the prototype's `json.ast`:

```
NREPEAT: sequence dict {
  choice {"sequence":@, "choice":@}:type=@,
  ?choice {"array":@, "dict":@}:container=@,
  <ITEMS>:items=@
}

REPEAT: sequence dict {
  "repeat":type=@,
  ?choice {"array":@, "dict":@}:container=@,
  <ITEM>:item=@,
  ?sequence {"separator", <ITEM>:separator=@}
}

CMD: sequence dict {
  ?"?":optional=true,
  choice {<NREPEAT>, <REPEAT>}
}
```

## 7. Mapping `.rawast` to the engine data model

Every `.rawast` construct maps to a Node in the in-memory grammar tree.
The mapping is direct:

| `.rawast` construct                | Node                                                |
| :---                               | :---                                                |
| `NAME: EXPR`                       | Registers EXPR's node under `NAME` in the grammar |
| `<NAME>`                           | `Node{kind=Ref, value="NAME"}`                     |
| `"text"`                           | `Node{kind=Key, value="text"}`                     |
| `"text":CONSTANT`                  | Key with a Value-kind child holding CONSTANT       |
| `identifier`                       | `Node{kind=Parse, value="identifier"}`             |
| `X:@=`                             | X with `is_name=true`                              |
| `X:name=@`                         | A sub-Sequence emitting (name-marker `name`, X)    |
| `X:name=<literal>`                 | (X parser, name-marker `name`, Value-kind literal) — X still parses, its value discarded; literal emitted instead |
| `sequence { items }`               | `Node{kind=Sequence, container=None}` + children   |
| `sequence array { items }`         | `Node{kind=Sequence, container=Array}` + children  |
| `sequence dict { items }`          | `Node{kind=Sequence, container=Dict}` + children   |
| `choice { items }`                 | `Node{kind=Choice}` + children                     |
| `repeat X`                         | `Node{kind=Repeat}` + [X]                          |
| `repeat X separator Y`             | `Node{kind=Repeat, has_separator=true}` + [Y, X]   |
| `?X`                               | X with `is_optional=true`                          |
| `X indent`                         | X with `depth_in=true` (depth+1 for its scope)    |
| `X tab`                            | X with `indent_emit=true` (emit indent at content) |
| `X space`                          | X with `space_after=true` (emit `" "` after)       |
| `X newline`                        | X with `newline_after=true` (emit `"\n"` after)    |
| `X tail="..."`                     | X with `tail` string (escape-interpreted)          |

After loading, the in-memory grammar tree is indistinguishable from one
loaded from an equivalent JSON file. The `.jast` container format does
not record which serialisation produced its bundled grammar, because
the data model is the only thing the container needs.

## 8. Implementation notes (forward references)

This format spec is finalised at the design level. Implementing the
`.rawast` loader requires:

1. **A `.rawast` grammar definition** (in JSON form initially, since the
   engine boostraps from JSON). The grammar produces an `Value` tree
   shaped identically to a JSON grammar file's parsed output.
2. **The JSON-grammar loader** — walks an `Value`
   tree and constructs a `Grammar` via the builder API.
3. **An `IdentifierParser` terminal** (`[a-zA-Z_][a-zA-Z0-9_]*`) with
   a "not followed by an identifier character" check, used to match
   reserved words (`sequence`, `choice`, `repeat`, etc.) without
   consuming partial identifiers like `sequencing`.
4. **Value-kind nodes with `is_name=true`** — the engine currently
   absorbs Value-kind children into the catcher with hardcoded
   `is_name=false`. To support the `name=@` binding form, the absorber
   needs to honour the Node's `is_name` flag.

Once these four pieces land, `.rawast` files can be loaded and the
engine self-hosts: future grammars are written in `.rawast` syntax,
parsed via the JSON-loaded `.rawast` grammar, and contribute to the
community grammar repository.

## 9. Confirmed design decisions

The following were open during drafting and are now settled:

- **Trailing commas inside `{ items }` lists are allowed.** Improves
  editor ergonomics; doesn't introduce ambiguity.
- **`start:` may appear anywhere in the file.** Not required to be the
  first rule definition. Convention: place at the top for readability,
  but the loader doesn't enforce it.
- **String literals are single-line.** Embedded literal newlines are
  rejected. Newlines inside strings can be expressed via escape
  passthrough — `"\n"` is the two characters backslash-`n`, the
  intended payload for higher-level escape interpretation.
- **Unterminated block comments are a parse error.** Reaching EOF
  inside a `/* …` comment fails the parse with a max-progress error
  pointing at the comment's opening position.
- **Pretty-print postfix attributes (`indent`, `tab`, `space`,
  `newline`, `tail="..."`) attach to the preceding expression with no
  separator.** They are part of the same item; commas separate sibling
  items (§4.5b). The save direction emits them in the fixed order tab
  → content → tail → space → newline.
- **`indent` bumps depth *before* `tab` fires** on the same Node, so
  `<X> tab indent` emits the indent at the bumped depth — the natural
  reading of "this item starts a deeper line."
- **`tail` strings are escape-interpreted at load time.** `\n`, `\t`,
  `\r`, `\\`, `\"`, `\0` are recognised; unknown escapes pass through
  verbatim. Use the bare `newline` flag for plain newlines and reserve
  `tail` for non-newline content (or for combinations like `tail="\\"
  newline` which emits backslash-newline for line-continuation
  grammars).

---

## Appendix: rule reference (auto-generated)

Rule reference for the `.rawast` meta-grammar itself, generated by
walking `grammars/rawast.rawast` and emitting EBNF for each rule.
Regenerate after grammar changes:

```sh
rawast docs grammars/rawast.rawast --title Rules --heading-level 3 \
    >> docs/rawast-format.md
```

Conventions:
- `"literal"` — keyword/punctuation matched by the `key` node kind.
- `Name` — reference to another rule in this file.
- `*name*` — terminal parser supplied by the `use:` group (e.g.
  `*string*`, `*identifier*` come from `std`).
- `A?` optional, `A*` zero-or-more, `A+` one-or-more.
- `A ( sep A )*` — a separator-form repeat.
- `A | B | C` — choice.

A companion view describing the *value-tree shape* the grammar produces
(dict fields, array element shapes, choice alternatives, optional /
constant bindings) is available via `rawast schema`:

```sh
rawast schema grammars/rawast.rawast --title "rawast — value-tree shape" \
    > docs/rawast-shape.md
```

The two views share a single source (the grammar dict): `rawast docs`
reads it as input syntax for consumers; `rawast schema` reads it as
output shape for producers.

### Rules

**Uses:** std

**Start:** `FILE`


#### BINDING

```ebnf
BINDING := ":" BIND_EXPR
```

#### BINDINGS

```ebnf
BINDINGS := BINDING+
```

#### BIND_EXPR

```ebnf
BIND_EXPR := NAME_BIND | VALUE_BIND
```

#### BIND_VAL

```ebnf
BIND_VAL := "@" | CONSTANT
```

#### CHOICE_EXPR

```ebnf
CHOICE_EXPR := "choice" "{" ITEMS "}"
```

#### CONSTANT

```ebnf
CONSTANT := *string* | *float* | *int* | "null" | "true" | "false"
```

#### CONTAINER_KIND

```ebnf
CONTAINER_KIND := "array" | "dict"
```

#### EXPR

```ebnf
EXPR := SEQUENCE_EXPR | CHOICE_EXPR | REPEAT_EXPR | KEY_EXPR | REF | PARSE_EXPR
```

#### FILE

```ebnf
FILE := FILE_ENTRY*
```

#### FILE_ENTRY

```ebnf
FILE_ENTRY := USE_DECL | START_DECL | RULE_DEF
```

#### IDENT_LIST

```ebnf
IDENT_LIST := *qualified_identifier* ( "," *qualified_identifier* )*
```

#### IGNORE_LIST

```ebnf
IGNORE_LIST := *qualified_identifier**
```

#### ITEM

```ebnf
ITEM := "?"? EXPR BINDINGS? POSTFIX_ATTR*
```

#### ITEMS

```ebnf
ITEMS := ITEM ( "," ITEM )*
```

#### KEY_EXPR

```ebnf
KEY_EXPR := *string* KEY_VALUE?
```

#### KEY_VALUE

```ebnf
KEY_VALUE := ":" ( "@" | CONSTANT )
```

#### NAME_BIND

```ebnf
NAME_BIND := "=" BIND_VAL
```

#### PARSE_EXPR

```ebnf
PARSE_EXPR := *qualified_identifier*
```

#### POSTFIX_ATTR

```ebnf
POSTFIX_ATTR := "indent" | "tab" | "space" | "newline" | TAIL_ATTR
```

#### REF

```ebnf
REF := "<" *identifier* ">"
```

#### REPEAT_EXPR

```ebnf
REPEAT_EXPR := "repeat" "+"? ITEM SEPARATOR?
```

#### RULE_BODY

```ebnf
RULE_BODY := RULE_IGNORE_ATTR? ":" "?"? EXPR BINDINGS? POSTFIX_ATTR*
```

#### RULE_DEF

```ebnf
RULE_DEF := *identifier* RULE_BODY
```

#### RULE_IGNORE_ATTR

```ebnf
RULE_IGNORE_ATTR := "ignore" IGNORE_LIST
```

#### SEPARATOR

```ebnf
SEPARATOR := "separator" ITEM
```

#### SEQUENCE_EXPR

```ebnf
SEQUENCE_EXPR := "sequence" CONTAINER_KIND? "{" ITEMS "}"
```

#### START_DECL

```ebnf
START_DECL := "start" ":" START_VAL
```

#### START_VAL

```ebnf
START_VAL := REF
```

#### TAIL_ATTR

```ebnf
TAIL_ATTR := "tail" "=" *string*
```

#### USE_DECL

```ebnf
USE_DECL := "use" ":" IDENT_LIST
```

#### VALUE_BIND

```ebnf
VALUE_BIND := *identifier* "=" BIND_VAL
```

