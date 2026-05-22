"""Walk a grammar dict and emit Markdown documentation with EBNF-style
rule bodies.

The grammar dict is the same shape produced by `rawast_format().parse_file(...)`
or json-loaded from a *.json grammar.

Quick usage:

    import rawast
    from rawast.docs import to_markdown

    meta = rawast.rawast_format()
    g = meta.parse_file(rawast.grammar_path("rawast.rawast"))
    print(to_markdown(g, title="rawast"))
"""

from __future__ import annotations

from typing import Any


def to_markdown(grammar: dict, *, title: str | None = None,
                heading_level: int = 1) -> str:
    """Render a grammar dict as Markdown with EBNF rule bodies.

    Top-level keys handled: `use`, `ignore`, `start`. Every other
    UPPER-CASE key is a rule whose value is an item dict.

    `heading_level` is the level of the document title (default 1, i.e.
    `# Title`). Per-rule headings are one level deeper. Bump it to nest
    the output under an existing section (e.g. 3 for `### Title` /
    `#### RuleName`).
    """
    rule_names = {k for k in grammar if _is_rule_name(k)}
    title_h = "#" * heading_level
    rule_h = "#" * (heading_level + 1)
    out: list[str] = []
    if title:
        out.append(f"{title_h} {title}\n")
    if use := grammar.get("use"):
        out.append(f"**Uses:** {', '.join(use)}\n")
    if ignore := grammar.get("ignore"):
        out.append(f"**Ignores:** {', '.join(ignore)}\n")
    if start := grammar.get("start"):
        start_name = start["type"] if isinstance(start, dict) else start
        out.append(f"**Start:** `{start_name}`\n")
    out.append("")

    for name in sorted(rule_names):
        body = grammar[name]
        out.append(f"{rule_h} {name}")
        out.append("")
        out.append("```ebnf")
        out.append(f"{name} := {_render(body, rule_names, top=True)}")
        out.append("```")
        out.append("")
    return "\n".join(out)


def _is_rule_name(k: str) -> bool:
    return k.isupper() or (k and k[0].isupper())


def _render(item: Any, rule_names: set[str], *, top: bool = False) -> str:
    """Render one item dict as an EBNF fragment."""
    if isinstance(item, str):
        # Wrapper form: {"expr": "RULE_NAME", ...} — the expr is a bare
        # rule-name string referencing another rule.
        return item if item in rule_names else f"*{item}*"
    if not isinstance(item, dict):
        return repr(item)

    # Wrapper form: {expr: <inner>, bindings?: [...]} — unwrap.
    if "expr" in item and "type" not in item:
        return _render(item["expr"], rule_names, top=top)

    t = item.get("type")
    optional = item.get("optional", False)

    if t == "sequence":
        items = item.get("value") or item.get("items") or []
        parts = [_render(c, rule_names) for c in items]
        s = " ".join(parts)
        if not top and (len(parts) > 1 or optional):
            s = f"( {s} )"
        if optional:
            s += "?"
        return s

    if t == "choice":
        items = item.get("value") or item.get("items") or []
        parts = [_render(c, rule_names) for c in items]
        s = " | ".join(parts)
        if not top:
            s = f"( {s} )"
        if optional:
            s += "?"
        return s

    if t == "repeat":
        body = item.get("value") if "value" in item else item.get("item", {})
        inner = _render(body, rule_names)
        suffix = "+" if item.get("min", 0) >= 1 else "*"
        sep = item.get("separator")
        if sep:
            sep_str = _render(sep, rule_names)
            s = f"{inner} ( {sep_str} {inner} ){suffix}"
        else:
            s = f"{inner}{suffix}"
        if optional:
            s = f"( {s} )?"
        return s

    if t == "key":
        key = item.get("key", "")
        s = f'"{key}"'
        if optional:
            s += "?"
        return s

    # Otherwise: a name. Either a rule reference (UPPER) or a terminal
    # parser (lowercase from `use:`).
    if isinstance(t, str):
        if t in rule_names:
            s = t
        else:
            # Italicize terminals to distinguish from rule refs.
            s = f"*{t}*"
        if optional:
            s += "?"
        return s

    return "?"
