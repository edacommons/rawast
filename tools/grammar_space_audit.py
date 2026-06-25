#!/usr/bin/env python3
"""Grammar space-audit: scan a .rawast grammar (default systemverilog)
for sequences where a word-emitter is immediately followed by a keyword/
name with no space/newline/tail — i.e. latent save-glue (e.g. the
`solve abefore` / `do begin`->`dobegin` bugs). Heuristic: [word] flags
are high-confidence; [ref] flags need an empirical round-trip check
(refs whose trailing emit is punctuation, or *_ITEMS whose newline is
internal, are false positives). Run from repo root."""
import rawast, json, re
mg = rawast.Grammar("rawast")
g = mg.parse_string(open("grammars/systemverilog.rawast").read())
g = g if isinstance(g, dict) else json.loads(g.to_json())

NAME_TERMS = {"sv_identifier","sv_qualified_type","sv_system_name"}
def is_word_key(el):
    k = el.get("key")
    return el.get("type") in ("strict_key","key") and k and re.match(r"[A-Za-z_`]", k)
def is_name(el):
    t = el.get("type","")
    return t in NAME_TERMS or t.startswith("sv_")
def is_ref(el):
    t = el.get("type","")
    return t[:1].isupper()  # rule ref
def is_repeat(el):
    return el.get("type") == "repeat"
def has_sep(el):
    return el.get("space") or el.get("newline") or ("tail" in el)
def emits_trailing_word(el):
    # word-emitter that ends in a word char (keyword/name), or ref/repeat (unknown)
    if is_word_key(el) or is_name(el): return "word"
    if is_ref(el) or is_repeat(el): return "ref"
    return None  # punctuation/other

flags = []
def walk_seq(rule, elems):
    for i in range(len(elems)-1):
        a, b = elems[i], elems[i+1]
        # b must START with a keyword (word) to risk a glue
        bstart_kw = is_word_key(b) or is_name(b)
        if not bstart_kw: continue
        if has_sep(a): continue
        kind = emits_trailing_word(a)
        if kind is None: continue
        flags.append((rule, kind, desc(a), desc(b)))
    # recurse into nested seq/choice/repeat values
    for el in elems:
        recurse(rule, el)
def desc(el):
    t = el.get("type","?")
    if t in ("strict_key","key"): return repr(el.get("key"))
    if t == "repeat":
        return "repeat<%s>" % desc(el.get("value",{}))
    return t
def recurse(rule, el):
    if not isinstance(el, dict): return
    if el.get("type") in ("sequence","choice") and isinstance(el.get("value"), list):
        if el.get("type")=="sequence": walk_seq(rule, el["value"])
        else:
            for alt in el["value"]: recurse(rule, alt)
    if el.get("type")=="repeat" and isinstance(el.get("value"), dict):
        recurse(rule, el["value"])

for rule, body in g.items():
    if not isinstance(body, dict): continue
    v = body.get("value")
    if body.get("type")=="sequence" and isinstance(v, list): walk_seq(rule, v)
    elif body.get("type")=="choice" and isinstance(v, list):
        for alt in v: recurse(rule, alt)

print(f"{len(flags)} adjacency flags (word-emitter -> keyword/name, no space):\n")
for rule, kind, a, b in flags:
    print(f"  [{kind:4}] {rule:28} {a}  ->  {b}")
