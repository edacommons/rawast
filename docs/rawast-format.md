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
indent    tab     space   newline    tail
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

### 4.4 Parser — `identifier`

A bare identifier (not in reserved words, not enclosed in `<...>`) names
a terminal parser registered on the grammar:

```
int        // SignedIntParser
uint       // UIntParser
float      // FloatParser
string     // DoubleQuoteStringParser
identifier // IdentifierParser  (a future built-in)
```

The set of available parsers is determined by what's been registered
when the grammar is loaded.

### 4.5 Binding — `expression:bind_target`

```
binding     ::= expression ':' bind_target
bind_target ::= '@'                              -- pass value through (default)
             |  '@='                             -- emit value as dict key
             |  identifier '=' '@'              -- emit (identifier, value) pair into dict
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

**`X:name=@`** is sugar for "emit the pair (constant string `name`,
value-of-X) into the surrounding dict catcher." This avoids needing a
separate name-marker terminal for fixed field names:

```
NREPEAT: sequence dict {
  choice {"sequence":@, "choice":@}:type=@,        // type field
  ?choice {"array":@, "dict":@}:container=@,       // optional container field
  <ITEMS>:items=@                                  // items field
}
```

In the JSON-grammar-format equivalent, the `name=@` binding desugars to
inserting a Value-kind name marker plus the wrapped expression — a small
engine extension required to support this form (see §8).

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
