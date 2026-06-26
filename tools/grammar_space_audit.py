#!/usr/bin/env python3
"""Grammar space-audit (static save-glue finder).

Walks a .rawast grammar and flags every sequence position where element A
is immediately followed by element B with NO separator (space/newline/tail)
between them, AND A can END with a word char [A-Za-z0-9_$] while B can
START with one — i.e. on save the two tokens would glue into one
(`logic`+`unsigned` -> `logicunsigned`, `input`+`i` -> `inputi`,
`int`+`unsigned` -> `intunsigned`, `iface`+`intf` -> `ifaceintf`).

Unlike a naive scan, ends_word(A)/starts_word(B) are computed by a
fixpoint THROUGH rule refs (so `<TYPE_SPEC>` after a keyword is caught,
and `<PACKED_DIMENSIONS>` — which ends in `]` — is NOT a false positive).
Optionals, repeats, choices, negative-lookaheads and internal trailing
spaces are all accounted for.

Run from repo root:  python tools/grammar_space_audit.py [grammar_name]
Exit code 1 if any flags (so it can gate CI).
"""
import rawast, json, re, sys

GRAMMAR = sys.argv[1] if len(sys.argv) > 1 else "systemverilog"
mg = rawast.Grammar("rawast")
g = mg.parse_string(open(f"grammars/{GRAMMAR}.rawast").read())
rules = g if isinstance(g, dict) else json.loads(g.to_json())

# Lowercase terminals whose emitted text is a word run (starts AND ends
# with a word char). `string` emits `"..."` (quote boundaries — not word).
WORD_TERMINALS = {
    "sv_identifier", "sv_qualified_type", "sv_system_name", "sv_based_digits",
    "sv_based_digits_dec", "identifier", "int", "integer", "float",
}
SEP_ATTRS = ("space", "newline", "tail")


def char_is_word(c):
    # Word chars that, when two are adjacent across a token boundary, fuse
    # into a single lexed token. NOT backtick: `\`name` is an intentional
    # macro-use sigil+name join, not a save-glue bug.
    return bool(c) and bool(re.match(r"[A-Za-z0-9_$]", c))


def has_sep(el):
    return any(el.get(a) for a in SEP_ATTRS)


# ---- fixpoint over rule starts_word / ends_word ------------------------
starts = {r: False for r in rules}
ends = {r: False for r in rules}


def el_starts(el):
    if el.get("negative"):
        return False                       # lookahead emits nothing
    t = el.get("type", "")
    if t == "strict_key":
        return char_is_word(el.get("key", "")[:1])
    if t == "key":
        return char_is_word(el.get("key", "")[:1])
    if t == "raw":
        return True                        # `*` consumes/emits arbitrary text
    if t in WORD_TERMINALS:
        return True
    if t == "string":
        return False
    if t == "repeat":
        return el_starts(el.get("value", {}))
    if t == "sequence":
        return seq_starts(el.get("value", []))
    if t == "choice":
        return any(el_starts(a) for a in el.get("value", []))
    if t in starts:                        # rule ref (Upper)
        return starts[t]
    return False                           # unknown lowercase terminal: assume punct


def el_ends(el):
    if el.get("negative"):
        return False
    if has_sep(el):
        return False                       # trailing space/newline/tail separates
    t = el.get("type", "")
    if t == "strict_key":
        return char_is_word(el.get("key", "")[-1:])
    if t == "key":
        return char_is_word(el.get("key", "")[-1:])
    if t == "raw":
        return True
    if t in WORD_TERMINALS:
        return True
    if t == "string":
        return False
    if t == "repeat":
        return el_ends(el.get("value", {}))
    if t == "sequence":
        return seq_ends(el.get("value", []))
    if t == "choice":
        return any(el_ends(a) for a in el.get("value", []))
    if t in ends:
        return ends[t]
    return False


def seq_starts(elems):
    res = False
    for el in elems:
        if el.get("negative"):
            continue
        res = res or el_starts(el)
        if not el.get("optional"):
            break                          # always emits -> nothing later is first
    return res


def seq_ends(elems):
    res = False
    for el in reversed(elems):
        if el.get("negative"):
            continue
        res = res or el_ends(el)
        if not el.get("optional"):
            break
    return res


def body_starts(body):
    t = body.get("type")
    if t == "sequence":
        return seq_starts(body.get("value", []))
    if t == "choice":
        return any(el_starts(a) for a in body.get("value", []))
    if t == "repeat":
        return el_starts(body.get("value", {}))
    return el_starts(body)


def body_ends(body):
    t = body.get("type")
    if t == "sequence":
        return seq_ends(body.get("value", []))
    if t == "choice":
        return any(el_ends(a) for a in body.get("value", []))
    if t == "repeat":
        return el_ends(body.get("value", {}))
    return el_ends(body)


changed = True
while changed:
    changed = False
    for r, body in rules.items():
        if not isinstance(body, dict):
            continue
        s, e = body_starts(body), body_ends(body)
        if s != starts[r]:
            starts[r] = s; changed = True
        if e != ends[r]:
            ends[r] = e; changed = True

# ---- walk every sequence and flag glue-risk adjacencies ----------------
flags = []


def desc(el):
    t = el.get("type", "?")
    if t in ("strict_key", "key"):
        return repr(el.get("key"))
    if t == "repeat":
        return "repeat<%s>" % desc(el.get("value", {}))
    return t


def walk_seq(rule, elems):
    for i in range(len(elems) - 1):
        a = elems[i]
        if has_sep(a) or a.get("negative"):
            continue
        if not el_ends(a):
            continue
        for j in range(i + 1, len(elems)):     # glue with the first emitter after A
            b = elems[j]
            if b.get("negative"):
                continue
            if el_starts(b):
                flags.append((rule, desc(a), desc(b)))
            break
    for el in elems:
        recurse(rule, el)


def recurse(rule, el):
    if not isinstance(el, dict):
        return
    t = el.get("type")
    if t == "sequence" and isinstance(el.get("value"), list):
        walk_seq(rule, el["value"])
    elif t == "choice" and isinstance(el.get("value"), list):
        for alt in el["value"]:
            recurse(rule, alt)
    elif t == "repeat" and isinstance(el.get("value"), dict):
        recurse(rule, el["value"])


for rule, body in rules.items():
    if not isinstance(body, dict):
        continue
    recurse(rule, body)

print(f"{len(flags)} save-glue risk(s) in {GRAMMAR} "
      f"(A ends word, B starts word, no separator):\n")
for rule, a, b in sorted(flags):
    print(f"  {rule:34} {a}  ->  {b}")
sys.exit(1 if flags else 0)
