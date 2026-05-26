"""Walk a grammar dict and emit Markdown documentation of the
*value-tree shape* the grammar produces.

Unlike `docs.to_markdown` (which renders the grammar's *input* syntax
as EBNF), this module describes what the parsed (or programmatically
constructed) Value tree looks like:

  - which rules produce dicts, arrays, or scalars,
  - what fields each dict-shaped rule contributes,
  - what element type lives inside an array,
  - what alternatives a choice can take,
  - what's optional.

Intended audience: developers who want to *produce* a value tree
programmatically (e.g. for grammar-driven save), or who want a
human-readable companion to the M3 Pydantic-model generator.

Quick usage:

    import rawast
    from rawast.schema import to_markdown

    meta = rawast.Grammar("rawast")
    g = meta.parse_file(rawast.grammar_path("lef.rawast"))
    print(to_markdown(g, title="LEF"))
"""

from __future__ import annotations

from typing import Any


def to_markdown(grammar: dict, *, title: str | None = None,
                heading_level: int = 1) -> str:
    """Render the value-tree shape of `grammar` as Markdown.

    `heading_level` is the level of the document title (default 1, i.e.
    `# Title`); per-rule headings sit one level deeper.
    """
    rule_names = {k for k in grammar if _is_rule_name(k)}
    title_h = "#" * heading_level
    rule_h = "#" * (heading_level + 1)

    out: list[str] = []
    if title:
        out.append(f"{title_h} {title} — value-tree shape")
        out.append("")

    start = grammar.get("start")
    start_name = start["type"] if isinstance(start, dict) else start
    if start_name:
        out.append(f"**Start rule:** `{start_name}` — the top-level value.")
        out.append("")

    out.append("**Notation.**")
    out.append("")
    out.append("- `RULE` — reference to another rule (see its own entry for shape).")
    out.append("- *terminal* — scalar value produced by a terminal parser (string / int / real / …).")
    out.append("- *dict value* — a dict whose listed keys come from this rule's bindings.")
    out.append("- *array value* — an ordered list whose element shape is listed.")
    out.append("- *one of …* — a discriminated alternative; the value matches exactly one option.")
    out.append("- `?` — optional (field absent or alternative not present).")
    out.append("")

    for name in sorted(rule_names):
        body = grammar[name]
        out.append(f"{rule_h} {name}")
        out.append("")
        out.extend(_describe(body, rule_names))
        out.append("")

    return "\n".join(out)


def _is_rule_name(k: str) -> bool:
    return k.isupper() or (k and k[0].isupper())


def _unwrap(item: Any) -> tuple[Any, list, bool]:
    """Unwrap an item from the `{expr, bindings?}` wrapper form.

    Returns `(inner_item, bindings, optional)` where bindings is the list
    attached to the outer wrapper (or to the item itself when not wrapped),
    and optional is the boolean attached there. The inner_item is the
    unwrapped payload.
    """
    if isinstance(item, dict) and "expr" in item and "type" not in item:
        return item["expr"], (item.get("bindings") or []), bool(item.get("optional", False))
    if isinstance(item, dict):
        return item, (item.get("bindings") or []), bool(item.get("optional", False))
    return item, [], False


def _describe(body: Any, rule_names: set[str]) -> list[str]:
    """Markdown lines describing this rule body's value shape."""
    if not isinstance(body, dict):
        return [f"Scalar: {body!r}"]

    container = body.get("container")
    t = body.get("type")

    if container == "dict":
        return _describe_dict(body, rule_names)
    if container == "array":
        return _describe_array(body, rule_names)
    if t == "choice":
        return _describe_choice(body, rule_names)
    if t == "sequence":
        return _describe_sequence(body, rule_names)
    if t == "repeat":
        return _describe_repeat(body, rule_names)
    if t == "key":
        return [f"Structural keyword `\"{body.get('key', '')}\"` — contributes no value."]
    if isinstance(t, str):
        if t in rule_names:
            return [f"Same shape as [`{t}`](#{_anchor(t)})."]
        return [f"Scalar value from terminal parser *{t}*."]
    return ["(unknown shape)"]


def _describe_dict(body: dict, rule_names: set[str]) -> list[str]:
    fields = _collect_field_contribs(body, rule_names)
    refs = _collect_refs(body, rule_names)
    out = ["*Dict value.*", ""]
    if fields:
        out.append("| Field | Type | Required |")
        out.append("|---|---|---|")
        for fname, ftype, opt in fields:
            req = "no" if opt else "yes"
            out.append(f"| `{fname}` | {ftype} | {req} |")
        out.append("")
    if refs:
        out.append("Fields may also be contributed by descended rules: "
                  + ", ".join(f"[`{r}`](#{_anchor(r)})" for r in refs)
                  + ".")
        out.append("")
    if not fields and not refs:
        out.append("(No directly-bound fields detected.)")
        out.append("")
    return out


def _describe_array(body: dict, rule_names: set[str]) -> list[str]:
    elem = _array_element_shape(body, rule_names)
    return [f"*Array value;* each element: {elem}.", ""]


def _describe_choice(body: dict, rule_names: set[str]) -> list[str]:
    items = body.get("value") or body.get("items") or []
    if not isinstance(items, list):
        items = [items]
    out = ["*One of:*", ""]
    for c in items:
        out.append(f"- {_shape_inline(c, rule_names)}")
    out.append("")
    return out


def _describe_sequence(body: dict, rule_names: set[str]) -> list[str]:
    fields = _collect_field_contribs(body, rule_names)
    refs = _collect_refs(body, rule_names)
    out: list[str] = []
    if fields:
        out.append("Contributes the following fields to its enclosing dict:")
        out.append("")
        out.append("| Field | Type | Required |")
        out.append("|---|---|---|")
        for fname, ftype, opt in fields:
            req = "no" if opt else "yes"
            out.append(f"| `{fname}` | {ftype} | {req} |")
        out.append("")
    if refs and not fields:
        out.append("Descends into: " + ", ".join(f"[`{r}`](#{_anchor(r)})" for r in refs) + ".")
        out.append("")
    if not fields and not refs:
        out.append("(Structural sequence; pass-through value.)")
        out.append("")
    return out


def _describe_repeat(body: dict, rule_names: set[str]) -> list[str]:
    inner = body.get("value") if "value" in body else body.get("item", {})
    min_n = body.get("min", 0)
    qualifier = "one-or-more" if min_n >= 1 else "zero-or-more"
    return [f"*Array value* ({qualifier}); each element: {_shape_inline(inner, rule_names)}.", ""]


def _shape_inline(item: Any, rule_names: set[str]) -> str:
    """One-line summary of a value shape, for use inside a bullet or row."""
    if isinstance(item, str):
        if item in rule_names:
            return f"[`{item}`](#{_anchor(item)})"
        return f"*{item}*"
    if not isinstance(item, dict):
        return repr(item)

    # Wrapper form
    if "expr" in item and "type" not in item:
        return _shape_inline(item["expr"], rule_names)

    t = item.get("type")
    container = item.get("container")

    if container == "dict":
        return "dict"
    if container == "array":
        elem = _array_element_shape(item, rule_names)
        return f"array of {elem}"

    if t == "choice":
        items = item.get("value") or item.get("items") or []
        if not isinstance(items, list):
            items = [items]
        return " \\| ".join(_shape_inline(c, rule_names) for c in items)

    if t == "repeat":
        body = item.get("value") if "value" in item else item.get("item", {})
        min_n = item.get("min", 0)
        marker = "+" if min_n >= 1 else "*"
        return f"array of {_shape_inline(body, rule_names)} ({marker})"

    if t == "key":
        return f'`"{item.get("key", "")}"`'

    if t == "sequence":
        return "(composite)"

    if isinstance(t, str):
        if t in rule_names:
            return f"[`{t}`](#{_anchor(t)})"
        return f"*{t}*"

    return "?"


def _collect_field_contribs(body: dict, rule_names: set[str]) -> list[tuple[str, str, bool]]:
    """For a rule body, walk its direct items and gather `(name, type, optional)`
    triples for every field-binding and var-marker (dict-key) binding.

    Handles both `{"var": true, "value": "@"}` and `{"name": "", "value": "@"}`
    var-marker conventions. A var-marker pairs with the next non-structural
    item (skipping `key`-type items, which are syntactic punctuation).
    """
    fields: list[tuple[str, str, bool]] = []
    items = body.get("value") or body.get("items") or []
    if not isinstance(items, list):
        items = [items]
    pending_name_marker: str | None = None
    pending_optional = False
    for child in items:
        inner, bindings, opt = _unwrap(child)
        is_var_marker = False
        for b in bindings:
            if not isinstance(b, dict):
                continue
            name = b.get("name")
            bv = b.get("value")
            if b.get("var") or name == "":
                is_var_marker = True
                pending_name_marker = _shape_inline(inner, rule_names)
                pending_optional = opt
                continue
            if not name:
                continue
            # Distinguish "captured value" (`value: "@"`) from a constant.
            if bv == "@":
                ftype = _shape_inline(inner, rule_names)
            else:
                ftype = _format_literal(bv)
            fields.append((name, ftype, opt))
        if is_var_marker:
            continue
        # Skip pure structural keys when looking for the value side of a
        # var-marker pairing.
        if pending_name_marker is not None and isinstance(inner, dict) \
                and inner.get("type") == "key" and not bindings:
            continue
        if pending_name_marker is not None:
            ftype = _shape_inline(inner, rule_names)
            fields.append((f"<key: {pending_name_marker}>", ftype, pending_optional or opt))
            pending_name_marker = None
            pending_optional = False
    return fields


def _format_literal(value: Any) -> str:
    """Format a constant-value binding as a markdown fragment."""
    if value is None:
        return "`null`"
    if isinstance(value, bool):
        return "`true`" if value else "`false`"
    if isinstance(value, (int, float)):
        return f"`{value}` (constant)"
    if isinstance(value, str):
        return f'`"{value}"` (constant)'
    return f"`{value!r}`"


def _collect_refs(body: dict, rule_names: set[str]) -> list[str]:
    """Names of rule references this body descends into (in source order, deduped)."""
    refs: list[str] = []
    seen: set[str] = set()

    def walk(item: Any) -> None:
        if isinstance(item, str):
            if item in rule_names and item not in seen:
                seen.add(item)
                refs.append(item)
            return
        if not isinstance(item, dict):
            return
        inner, _, _ = _unwrap(item)
        if isinstance(inner, str):
            if inner in rule_names and inner not in seen:
                seen.add(inner)
                refs.append(inner)
            return
        if not isinstance(inner, dict):
            return
        t = inner.get("type")
        if isinstance(t, str) and t in rule_names and t not in seen:
            seen.add(t)
            refs.append(t)
        children = inner.get("value") or inner.get("items") or []
        if isinstance(children, list):
            for c in children:
                walk(c)
        elif isinstance(children, dict):
            walk(children)
        sep = inner.get("separator")
        if isinstance(sep, dict):
            walk(sep)
        if "item" in inner:
            walk(inner["item"])

    walk(body)
    return refs


def _array_element_shape(body: dict, rule_names: set[str]) -> str:
    """For a `container: array` rule, find the inner element shape."""
    items = body.get("value") or body.get("items") or []
    if not isinstance(items, list):
        items = [items]
    for child in items:
        inner, _, _ = _unwrap(child)
        if isinstance(inner, dict) and inner.get("type") == "repeat":
            elem = inner.get("value") if "value" in inner else inner.get("item", {})
            elem_inner, _, _ = _unwrap(elem)
            return _shape_inline(elem_inner, rule_names)
    return "?"


def _anchor(name: str) -> str:
    """GitHub-flavoured-Markdown anchor for a heading."""
    return name.lower().replace("_", "-").replace(" ", "-")
