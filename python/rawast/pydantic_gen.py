"""Generate Pydantic v2 model source from a rawast grammar.

The generated models match the grammar's parse/save dict shape
**exactly**. The user can construct only what the grammar can save;
parsed dicts validate as models without coercion; round-trip is:

    Class.model_validate(g.parse_file(path)).model_dump(...) == g.parse_file(path)

This is the construction-toolkit contract: build LEF/DEF models in
Python with autocomplete + required-field enforcement, dump to dict,
hand to `g.save(...)`.

Design rules (apply to every grammar, no per-format special cases):

  - **Container-less sequence rules** (no `container: "dict"` /
    `"array"`) are NOT emitted as standalone classes. Their bindings
    flatten into the dict-container parent rule(s) that reference
    them — that's how the engine's catcher mechanism works at parse
    time, so the Pydantic shape mirrors it.
  - **Catcher-only choice rules** (every branch resolves to a skipped
    rule, e.g. `SITE_PROPERTY: choice { <SITE_CLASS>, <SITE_SIZE>, ... }`)
    are also NOT emitted. Their branches' fields are hoisted during
    the parent's recursive field-collection.
  - **Dict-container sequence rules** become `BaseModel` classes.
    Their fields are the union of:
      - direct `:name=@` / `:name="const"` bindings on body items,
      - recursive catcher-flatten of any unnamed rule reference,
        unnamed repeat, or unnamed inline choice.
    Catcher-contributed fields are always Optional.
  - **Array-container sequence rules** become `X = list[ElementType]`
    aliases.
  - **Choice rules with any non-skipped branch** become `Union`
    aliases — discriminated via `Field(discriminator="<name>")` when
    every branch is a model class sharing a constant-bound field on
    that name (the `:type="Foo"` pattern in LEF/DEF and GDSII).
  - **`ConfigDict(extra="forbid")`** on every `BaseModel` enforces
    the contract: a model cannot accept fields the grammar can't
    write back.
  - **Python-keyword field names** (`class`, `from`, …) are aliased
    via `Field(alias="…")`; users pass them either as the renamed
    attribute (`class_`) or via `**{"class": …}`.

Cyclic alias groups (e.g. LEF `LayerValue ↔ LayerParenGroup` if both
remain after the catcher pass) use `TypeAliasType` from
`typing_extensions` so Pydantic treats them as named recursive types
rather than expanding indefinitely.
"""

from __future__ import annotations

from typing import Any


_TERMINAL_TYPES: dict[str, str] = {
    "string": "str",
    "identifier": "str",
    "qualified_identifier": "str",
    "int": "int",
    "float": "float",
    "real": "float",
}

_PY_KEYWORDS = {
    "False", "None", "True", "and", "as", "assert", "async", "await",
    "break", "class", "continue", "def", "del", "elif", "else", "except",
    "finally", "for", "from", "global", "if", "import", "in", "is",
    "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try",
    "while", "with", "yield",
}


# ─── public entry point ──────────────────────────────────────────────


def to_pydantic(grammar: dict, *, module_doc: str = "") -> str:
    """Generate Pydantic v2 source code for `grammar`."""
    rule_names = {k for k in grammar if _is_rule_name(k)}
    classifications = _classify_all(grammar, rule_names)

    # Record which alias names resolve to a list-shaped type so the
    # field emitter can default them to `Field(default_factory=list)`.
    _LIST_ALIAS_NAMES.clear()
    for name in rule_names:
        if classifications[name] == "list_alias":
            _LIST_ALIAS_NAMES.add(_class_name(name))

    # Pre-collect fields for each model rule (recursive catcher walk).
    model_fields: dict[str, list[_Field]] = {}
    for name in rule_names:
        if classifications[name] == "model":
            fields = _collect_fields_deep(
                grammar[name], rule_names, grammar, classifications,
                optional_ctx=False, visited=frozenset())
            model_fields[name] = _dedupe(fields)

    doc = module_doc or "Auto-generated Pydantic models from a rawast grammar."
    header = [
        f'"""{doc}',
        "",
        "DO NOT EDIT — regenerate with `rawast pydantic <grammar>`.",
        '"""',
        "",
        "from __future__ import annotations",
        "",
        "from typing import Annotated, Any, Literal, Union",
        "",
        "from pydantic import BaseModel, ConfigDict, Field",
        "from typing_extensions import TypeAliasType",
        "",
    ]

    # Emit model classes first (alphabetical) — class field annotations
    # are lazy under `from __future__ import annotations`, so cross-class
    # references and alias references resolve via `model_rebuild()` at
    # the end of the module.
    class_blocks: list[str] = []
    for name in sorted(rule_names):
        if classifications[name] == "model":
            class_blocks.append(_emit_model_class(name, model_fields[name]))

    # Aliases: topo-sort acyclic ones first (clean output); wrap cyclic
    # ones in TypeAliasType so they're named recursive types.
    alias_names = {n for n, c in classifications.items()
                   if c in ("union", "list_alias", "scalar_alias")}
    ordered, cyclic = _topo_sort_aliases(alias_names, rule_names, grammar)
    alias_blocks: list[str] = []
    for name in ordered:
        alias_blocks.append(_emit_alias(
            name, grammar[name], rule_names, grammar, classifications,
            recursive=False))
    for name in cyclic:
        alias_blocks.append(_emit_alias(
            name, grammar[name], rule_names, grammar, classifications,
            recursive=True))

    rebuild = ["", "# Resolve forward references."]
    for name in sorted(rule_names):
        if classifications[name] == "model":
            rebuild.append(f"{_class_name(name)}.model_rebuild()")

    return "\n".join(header + class_blocks + alias_blocks + rebuild) + "\n"


# ─── classification ──────────────────────────────────────────────────


def _classify_all(grammar: dict, rule_names: set[str]) -> dict[str, str]:
    """Two-pass classification.

    Pass 1: every rule by its own shape (model / union / list_alias /
    scalar_alias / skip).
    Pass 2: a choice is downgraded to `skip` when every branch resolves
    to an already-skipped rule (catcher-only — the choice's job is to
    flatten branch fields into the parent dict, not to be a public type).
    """
    out = {name: _classify_single(grammar[name], rule_names) for name in rule_names}
    changed = True
    while changed:
        changed = False
        for name in rule_names:
            if out[name] == "union":
                if _all_branches_skip(grammar[name], rule_names, grammar, out):
                    out[name] = "skip"
                    changed = True
    return out


def _classify_single(body: Any, rule_names: set[str]) -> str:
    if not isinstance(body, dict):
        return "skip"
    if body.get("container") == "dict":
        return "model"
    if body.get("container") == "array":
        return "list_alias"
    t = body.get("type")
    if t == "choice":
        return "union"
    if t == "sequence":
        return "skip"  # container-less — fields flatten into parent
    if t == "repeat":
        return "list_alias"
    if isinstance(t, str):
        return "scalar_alias"
    return "skip"


def _all_branches_skip(body: dict, rule_names: set[str], grammar: dict,
                       classifications: dict[str, str]) -> bool:
    """True if every branch of `body` (a choice) is itself skip-classified.

    A branch with a binding (`<X>:foo=@`) counts as 'flattenable' too —
    the binding contributes a single field to the parent on match.
    """
    branches = _items_of(body)
    if not branches:
        return False
    for branch in branches:
        inner, bindings, _ = _unwrap(branch)
        named = [b for b in (bindings or [])
                 if isinstance(b, dict) and b.get("name") and not b.get("var")]
        if named:
            continue  # flattens via the binding
        rule_name = _resolve_to_rule_name(inner, rule_names)
        if rule_name is None:
            # Inline expr — only "skippable" if it's a structural key
            # with no value attribute.
            if isinstance(inner, dict) and inner.get("type") == "key" \
                    and "value" not in inner:
                continue
            return False
        if classifications.get(rule_name) != "skip":
            return False
    return True


# ─── field collection ────────────────────────────────────────────────


_Field = tuple  # (name: str, type_str: str, optional: bool, default_expr: str | None)


def _collect_fields_deep(body: Any, rule_names: set[str], grammar: dict,
                         classifications: dict[str, str], *,
                         optional_ctx: bool, visited: frozenset[str]
                         ) -> list[_Field]:
    """Collect direct + catcher-flattened fields for a `body`.

    Dispatch by body shape:
      - sequence: walk items left-to-right
      - choice: walk every branch (every contribution is Optional)
      - anything else: wrap in a single virtual item and process
    """
    if not isinstance(body, dict):
        return []
    t = body.get("type")
    if t == "choice":
        out: list[_Field] = []
        for branch in _items_of(body):
            out.extend(_collect_from_item(
                branch, rule_names, grammar, classifications,
                optional_ctx=True, visited=visited))
        return out
    if t == "sequence":
        out = []
        for item in _items_of(body):
            out.extend(_collect_from_item(
                item, rule_names, grammar, classifications,
                optional_ctx=optional_ctx, visited=visited))
        return out
    # Single-expression body (rule ref, terminal, repeat). Wrap and process.
    return _collect_from_item(
        body, rule_names, grammar, classifications,
        optional_ctx=optional_ctx, visited=visited)


def _collect_from_item(item: Any, rule_names: set[str], grammar: dict,
                       classifications: dict[str, str], *,
                       optional_ctx: bool, visited: frozenset[str]
                       ) -> list[_Field]:
    """Field contributions from a single grammar item."""
    inner, bindings, item_optional = _unwrap(item)
    is_optional = optional_ctx or item_optional
    out: list[_Field] = []

    # Direct named bindings own the item's field contribution.
    named = [b for b in (bindings or [])
             if isinstance(b, dict) and b.get("name") and not b.get("var")]
    if named:
        for b in named:
            fname = b["name"]
            value = b["value"]
            # `list_append: true` flag (engine `[]` suffix on the name)
            # turns the field into an optional `list[X]`. The engine
            # creates the list lazily on first match, so a PIN with no
            # ANTENNA clauses has no `antennas` key in the parsed dict.
            # Modelling as `list[X] | None = None` lets `exclude_none`
            # round-trip both shapes (absent ↔ None, list ↔ list).
            is_list_append = bool(b.get("list_append"))
            if value == "@":
                inner_type = _type_of(inner, rule_names, grammar,
                                      classifications)
                if is_list_append:
                    out.append((fname, f"list[{inner_type}]", True, None))
                else:
                    out.append((fname, inner_type, is_optional, None))
            else:
                lit, default = _literal_type_and_default(value)
                if is_list_append:
                    out.append((fname, f"list[{lit}]", True, None))
                else:
                    # Catcher-flattened constants (optional context, e.g.
                    # `FIXEDMASK:fixed_mask=true` inside a `repeat <CHOICE>`)
                    # default to None, not the literal: the literal is only
                    # produced when the catcher branch actually fires.
                    # Direct-binding constants (e.g. `"SITE":type="Site"` on
                    # the SITE_BLOCK opener) keep the literal default
                    # because the parsed dict always has them.
                    out.append((fname, lit, is_optional,
                                None if is_optional else default))
        return out

    # No bindings — possibly catcher-flatten, possibly structural-skip.

    # Structural key without a value attribute contributes nothing.
    if isinstance(inner, dict) and inner.get("type") == "key" \
            and "value" not in inner:
        return out

    # Rule reference (bare string OR {type: "RULE_NAME"})
    rule_name = _resolve_to_rule_name(inner, rule_names)
    if rule_name is not None and rule_name not in visited:
        target = grammar[rule_name]
        new_visited = visited | {rule_name}
        if not isinstance(target, dict):
            return out
        # Walk-in decision is based on what the target IS, not its
        # `classify` bucket: choice and container-less sequence are
        # structural and need their bodies walked (per-branch / per-
        # item, the recursion handles `skip`-vs-`model` correctly).
        # Dict-container or array-container without a binding is
        # discarded by the engine, so contributes nothing.
        container = target.get("container")
        if container in ("dict", "array"):
            return out
        t = target.get("type")
        if t in ("choice", "sequence"):
            return _collect_fields_deep(
                target, rule_names, grammar, classifications,
                optional_ctx=is_optional, visited=new_visited)
        # Repeat alias, scalar alias, terminal alias: unbound → discarded.
        return out

    # Inline expressions
    if isinstance(inner, dict):
        t = inner.get("type")
        if t == "repeat":
            rep_item = inner.get("value")
            rep_inner, rep_bindings, _ = _unwrap(rep_item)
            rep_named = [b for b in (rep_bindings or [])
                         if isinstance(b, dict) and b.get("name") and not b.get("var")]
            if rep_named:
                for b in rep_named:
                    fname = b["name"]
                    value = b["value"]
                    if value == "@":
                        inner_type = _type_of(
                            rep_inner, rule_names, grammar, classifications)
                        # `repeat <X>:foo[]=@` matches the engine's
                        # list-append semantic: absent key when zero
                        # matches. Model as `list[X] | None = None`
                        # so the round-trip round-trips both shapes
                        # (absent ↔ None, list ↔ list). Same handling
                        # as the single-Ref `:foo[]=@` path above.
                        is_list_append = bool(b.get("list_append"))
                        opt = is_list_append or is_optional
                        out.append((fname, f"list[{inner_type}]", opt, None))
                return out
            # Unnamed repeat — same walk-in rule as bare reference:
            # walk choice and container-less sequence bodies; skip
            # dict-container / array (those would be discarded).
            rep_rule = _resolve_to_rule_name(rep_inner, rule_names)
            if rep_rule is not None and rep_rule not in visited:
                target = grammar[rep_rule]
                if isinstance(target, dict):
                    container = target.get("container")
                    t_target = target.get("type")
                    if container not in ("dict", "array") \
                            and t_target in ("choice", "sequence"):
                        return _collect_fields_deep(
                            target, rule_names, grammar, classifications,
                            optional_ctx=True, visited=visited | {rep_rule})
            return out
        if t == "choice":
            return _collect_fields_deep(
                inner, rule_names, grammar, classifications,
                optional_ctx=True, visited=visited)
        if t == "sequence":
            return _collect_fields_deep(
                inner, rule_names, grammar, classifications,
                optional_ctx=is_optional, visited=visited)

    return out


def _split_union(s: str) -> list[str]:
    """Split a Python type-annotation string on top-level ` | `,
    respecting `[]` bracket depth. Needed because the naive
    `.split(' | ')` mis-splits nested unions like `list[A | B]`."""
    parts: list[str] = []
    depth = 0
    buf = ""
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == '[':
            depth += 1
            buf += c
        elif c == ']':
            depth -= 1
            buf += c
        elif depth == 0 and s[i:i + 3] == ' | ':
            parts.append(buf.strip())
            buf = ""
            i += 3
            continue
        else:
            buf += c
        i += 1
    if buf.strip():
        parts.append(buf.strip())
    return parts


def _merge_union_types(a: str, b: str) -> str:
    """Build the merged Python type for two same-name field
    contributions. Either side may already be a union.

    Special case: if EVERY part on both sides is a `list[X]`, the
    result is `list[X | Y | …]` (list of union) instead of
    `list[X] | list[Y] | …` (union of lists) — see _dedupe for
    why heterogeneous lists need the inner-union form."""
    outer_parts: set[str] = set()
    inner_parts: set[str] = set()
    all_lists = True
    for t in (a, b):
        for piece in _split_union(t):
            if not piece:
                continue
            outer_parts.add(piece)
            if piece.startswith("list[") and piece.endswith("]"):
                # Flatten any nested union inside the list.
                inner = piece[len("list["):-1]
                for ip in _split_union(inner):
                    if ip:
                        inner_parts.add(ip)
            else:
                all_lists = False
    if all_lists and inner_parts:
        return f"list[{' | '.join(sorted(inner_parts))}]"
    return " | ".join(sorted(outer_parts))


def _dedupe(fields: list[_Field]) -> list[_Field]:
    """Merge same-name field entries.

    Two scenarios where one name appears multiple times:

      1. Different Choice branches of a catcher-flatten rule all
         bind the same name (e.g. PROPDEF_DEFAULT in lefdef.rawast
         is `choice { string:default_value=@, <NUMBER>:default_value=@ }`).
         Each branch contributes a _Field with the same name but a
         different type. The runtime dict ends up holding whichever
         branch fired, so the Python field type must be the UNION
         of every branch's contribution.

      2. Sequential same-name writes inside one rule body. The
         engine's dict-merge does last-write-wins, so the latest
         entry's type/default/optional flag wins. (Rare — only
         shows up in hand-crafted grammars; the spec-aligned
         rules avoid it.)

    Distinguishing the two via static analysis is tricky, so we
    merge structurally: if the types match, last-write-wins; if
    they differ, union (preserving `None` from the optional flag).
    Either behaviour is correct for the engine; the union case
    just yields a slightly broader Python type."""
    by_name: dict[str, _Field] = {}
    for f in fields:
        fname, ftype, optional, default = f
        if fname not in by_name:
            by_name[fname] = f
            continue
        prev_name, prev_type, prev_optional, prev_default = by_name[fname]
        if prev_type == ftype:
            # Same type — last-write-wins, OR-ing optional so any
            # branch that's optional makes the field optional.
            by_name[fname] = (
                fname, ftype, prev_optional or optional, default)
        else:
            # Different types from sibling Choice branches — emit
            # the union. Sort for stable output across runs.
            #
            # Special case: when EVERY contributing type is `list[X]`,
            # produce `list[X | Y | …]` (list of union) rather than
            # `list[X] | list[Y] | …` (union of lists). The former
            # validates each list element against the inner union;
            # the latter forces Pydantic to pick one list-type and
            # validate every element against ONE inner type, which
            # fails when a `repeat <Choice>:list[]=@` produces a
            # heterogeneous list (e.g. DEF VIA clauses where every
            # `+`-prefixed clause-type goes into the same `clauses`
            # list).
            merged = _merge_union_types(prev_type, ftype)
            by_name[fname] = (
                fname, merged, prev_optional or optional, prev_default)
    return list(by_name.values())


# ─── emit blocks ─────────────────────────────────────────────────────


def _emit_model_class(rule_name: str, fields: list[_Field]) -> str:
    cls = _class_name(rule_name)
    lines = [f"class {cls}(BaseModel):"]
    lines.append(f'    """Generated from grammar rule `{rule_name}`."""')
    lines.append('    model_config = ConfigDict(extra="forbid")')
    if not fields:
        lines.append("    pass")
        return "\n".join(lines) + "\n"
    lines.append("")
    for fname, ftype, optional, default in fields:
        safe = _safe_field_name(fname)
        needs_alias = safe != fname
        # Required field with constant default: defaulted but always set.
        # Optional field: `T | None = None` (or constant default if any).
        # Required field with no default: just `T`.
        if default is not None and not optional:
            if needs_alias:
                lines.append(
                    f'    {safe}: {ftype} = Field({default}, alias="{fname}")')
            else:
                lines.append(f"    {safe}: {ftype} = {default}")
        elif optional:
            default_expr = default if default is not None else "None"
            if needs_alias:
                lines.append(
                    f'    {safe}: {ftype} | None = Field({default_expr}, alias="{fname}")')
            else:
                lines.append(f"    {safe}: {ftype} | None = {default_expr}")
        else:
            # Required field. If the type is a list (either inline
            # `list[X]` or a named array alias), default to an empty
            # list — the engine's array-bound repeat produces `[]` when
            # the repeat matches zero times, so the parsed dict always
            # has the key but the value can be empty.
            list_default = _list_type_default(ftype)
            if list_default is not None:
                if needs_alias:
                    lines.append(
                        f'    {safe}: {ftype} = Field('
                        f'default_factory=list, alias="{fname}")')
                else:
                    lines.append(
                        f"    {safe}: {ftype} = Field(default_factory=list)")
            elif needs_alias:
                lines.append(
                    f'    {safe}: {ftype} = Field(..., alias="{fname}")')
            else:
                lines.append(f"    {safe}: {ftype}")
    return "\n".join(lines) + "\n"


_LIST_ALIAS_NAMES: set[str] = set()


def _list_type_default(ftype: str) -> str | None:
    """Return a non-None marker when the field type is a list shape, so
    the emitter can default it to `Field(default_factory=list)` rather
    than require explicit construction. Covers inline `list[X]` and
    named array-alias references (e.g. `MacroPropertiesTrail`)."""
    if ftype.startswith("list["):
        return "list"
    if ftype in _LIST_ALIAS_NAMES:
        return "list"
    return None


def _emit_alias(rule_name: str, body: Any, rule_names: set[str],
                grammar: dict, classifications: dict[str, str], *,
                recursive: bool) -> str:
    """Emit a type alias. `recursive=True` wraps the body in TypeAliasType
    so cyclic alias groups resolve lazily."""
    cls = _class_name(rule_name)
    if not isinstance(body, dict):
        return f"{cls} = Any  # unrecognised shape\n"

    container = body.get("container")
    t = body.get("type")

    if container == "array":
        elem = _array_element_type(body, rule_names, grammar, classifications)
        body_expr = f"list[{elem}]"
    elif t == "choice":
        return _emit_choice_alias(cls, body, rule_names, grammar,
                                  classifications, recursive=recursive)
    elif t == "repeat":
        inner = body.get("value")
        unwrapped, _, _ = _unwrap(inner)
        body_expr = f"list[{_type_of(unwrapped, rule_names, grammar, classifications)}]"
    elif isinstance(t, str):
        if t in rule_names:
            inner_cls = classifications.get(t, "skip")
            body_expr = "Any" if inner_cls == "skip" else _class_name(t)
        else:
            body_expr = _terminal_type(t)
    else:
        return f"{cls} = Any  # no fields detected\n"

    if recursive:
        return f'{cls} = TypeAliasType("{cls}", "{body_expr}")\n'
    return f"{cls} = {body_expr}\n"


def _emit_choice_alias(cls: str, body: dict, rule_names: set[str],
                       grammar: dict, classifications: dict[str, str], *,
                       recursive: bool) -> str:
    items = _items_of(body)
    if not items:
        return f"{cls} = Any  # empty choice\n"
    variant_types = [_type_of(_unwrap(it)[0], rule_names, grammar, classifications)
                     for it in items]
    unique = list(dict.fromkeys(variant_types))
    if len(unique) == 1:
        if recursive:
            return f'{cls} = TypeAliasType("{cls}", "{unique[0]}")\n'
        return f"{cls} = {unique[0]}\n"
    types_str = ", ".join(unique)

    discriminator = _detect_discriminator(items, rule_names, grammar,
                                          classifications)
    if recursive:
        return f'{cls} = TypeAliasType("{cls}", "Union[{types_str}]")\n'
    if discriminator:
        return (
            f"{cls} = Annotated[\n"
            f"    Union[{types_str}],\n"
            f'    Field(discriminator="{discriminator}"),\n'
            f"]\n"
        )
    return f"{cls} = Union[{types_str}]\n"


# ─── type resolution ─────────────────────────────────────────────────


def _type_of(item: Any, rule_names: set[str], grammar: dict,
             classifications: dict[str, str]) -> str:
    """Resolve `item` to a Python type expression string."""
    if isinstance(item, str):
        if item in rule_names:
            return _ref_class_name(item, classifications)
        return _terminal_type(item)
    if not isinstance(item, dict):
        return "Any"
    if "expr" in item and "type" not in item:
        return _type_of(item["expr"], rule_names, grammar, classifications)

    t = item.get("type")
    container = item.get("container")

    # Inline key with a constant `value` attribute (ON_OFF-style emit).
    if t == "key" and "value" in item:
        return _literal_for(item["value"])

    if container == "array":
        for it in _items_of(item):
            inner, _, _ = _unwrap(it)
            if isinstance(inner, dict) and inner.get("type") == "repeat":
                rep = inner.get("value")
                rep_inner, _, _ = _unwrap(rep)
                return f"list[{_type_of(rep_inner, rule_names, grammar, classifications)}]"
        return "list[Any]"

    if container == "dict":
        return "dict"

    if t == "repeat":
        inner = item.get("value")
        unwrapped, _, _ = _unwrap(inner)
        return f"list[{_type_of(unwrapped, rule_names, grammar, classifications)}]"

    if t == "choice":
        items = _items_of(item)
        if not items:
            return "Any"
        types = [_type_of(_unwrap(c)[0], rule_names, grammar, classifications)
                 for c in items]
        unique = list(dict.fromkeys(types))
        if len(unique) == 1:
            return unique[0]
        return f"Union[{', '.join(unique)}]"

    if t == "key":
        return "str"

    if isinstance(t, str):
        if t in rule_names:
            return _ref_class_name(t, classifications)
        return _terminal_type(t)

    return "Any"


def _ref_class_name(rule_name: str, classifications: dict[str, str]) -> str:
    cls = classifications.get(rule_name, "skip")
    if cls == "skip":
        return "Any"  # skipped rule — no class to reference
    return _class_name(rule_name)


def _array_element_type(body: dict, rule_names: set[str], grammar: dict,
                        classifications: dict[str, str]) -> str:
    for child in _items_of(body):
        inner, _, _ = _unwrap(child)
        if isinstance(inner, dict) and inner.get("type") == "repeat":
            rep = inner.get("value")
            rep_inner, _, _ = _unwrap(rep)
            return _type_of(rep_inner, rule_names, grammar, classifications)
    return "Any"


def _terminal_type(name: str) -> str:
    return _TERMINAL_TYPES.get(name, "Any")


def _literal_type_and_default(value: Any) -> tuple[str, str]:
    """Map a constant value (from `:name="X"` / `:name=true` / …) to
    (Pydantic type, Python source for the default)."""
    return (_literal_for(value), _literal_default(value))


def _literal_for(value: Any) -> str:
    if value is None:
        return "None"
    if isinstance(value, bool):
        return f"Literal[{value}]"
    if isinstance(value, int):
        return f"Literal[{value}]"
    if isinstance(value, float):
        return "float"
    if isinstance(value, str):
        return f'Literal["{_escape(value)}"]'
    return "Any"


def _literal_default(value: Any) -> str:
    if value is None:
        return "None"
    if isinstance(value, bool):
        return "True" if value else "False"
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, str):
        return f'"{_escape(value)}"'
    return repr(value)


def _escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


# ─── discriminator detection ─────────────────────────────────────────


def _detect_discriminator(items: list, rule_names: set[str], grammar: dict,
                          classifications: dict[str, str]) -> str | None:
    """If every choice branch references a model rule and all of those
    models share a constant-bound field on the SAME name, return that
    name as the discriminator."""
    if not items:
        return None
    common: set[str] | None = None
    for item in items:
        inner, _, _ = _unwrap(item)
        rule_name = _resolve_to_rule_name(inner, rule_names)
        if rule_name is None or classifications.get(rule_name) != "model":
            return None
        const_fields = _find_constant_fields(grammar[rule_name])
        if not const_fields:
            return None
        names = set(const_fields.keys())
        common = names if common is None else common & names
        if not common:
            return None
    if not common:
        return None
    return "type" if "type" in common else next(iter(sorted(common)))


def _find_constant_fields(body: dict) -> dict[str, Any]:
    """Names of direct-binding fields whose value is a constant (not `@`)."""
    out: dict[str, Any] = {}
    for child in _items_of(body):
        _, bindings, _ = _unwrap(child)
        for b in bindings or []:
            if not isinstance(b, dict):
                continue
            name = b.get("name")
            value = b.get("value")
            if b.get("var") or not name:
                continue
            if value != "@":
                out[name] = value
    return out


# ─── alias topo sort ─────────────────────────────────────────────────


def _refs_in_body(body: Any, rule_names: set[str], out: set[str]) -> None:
    if isinstance(body, str):
        if body in rule_names:
            out.add(body)
        return
    if not isinstance(body, dict):
        return
    if "expr" in body and "type" not in body:
        _refs_in_body(body["expr"], rule_names, out)
        for b in body.get("bindings") or []:
            if isinstance(b, dict):
                _refs_in_body(b.get("value"), rule_names, out)
        return
    t = body.get("type")
    if isinstance(t, str) and t in rule_names:
        out.add(t)
    children = body.get("value") or body.get("items") or []
    if isinstance(children, list):
        for c in children:
            _refs_in_body(c, rule_names, out)
    elif isinstance(children, dict):
        _refs_in_body(children, rule_names, out)
    if "item" in body:
        _refs_in_body(body["item"], rule_names, out)
    if "separator" in body:
        _refs_in_body(body["separator"], rule_names, out)


def _topo_sort_aliases(alias_set: set[str], rule_names: set[str],
                       grammar: dict) -> tuple[list[str], list[str]]:
    deps: dict[str, set[str]] = {}
    for name in alias_set:
        refs: set[str] = set()
        _refs_in_body(grammar[name], rule_names, refs)
        deps[name] = refs & alias_set

    ordered: list[str] = []
    emitted: set[str] = set()
    remaining = sorted(alias_set)
    while remaining:
        next_remaining: list[str] = []
        any_progress = False
        for name in remaining:
            if deps[name] <= emitted:
                ordered.append(name)
                emitted.add(name)
                any_progress = True
            else:
                next_remaining.append(name)
        if not any_progress:
            return ordered, next_remaining
        remaining = next_remaining
    return ordered, []


# ─── small helpers ───────────────────────────────────────────────────


def _is_rule_name(k: str) -> bool:
    return bool(k) and k[0].isupper()


def _class_name(rule_name: str) -> str:
    """UPPER_SNAKE_CASE → PascalCase."""
    return "".join(p[:1].upper() + p[1:].lower()
                   for p in rule_name.split("_") if p)


def _safe_field_name(name: str) -> str:
    if name in _PY_KEYWORDS:
        return name + "_"
    return name


def _items_of(body: dict) -> list:
    items = body.get("value") or body.get("items") or []
    if not isinstance(items, list):
        items = [items]
    return items


def _unwrap(item: Any) -> tuple[Any, list, bool]:
    if isinstance(item, dict) and "expr" in item and "type" not in item:
        return (item["expr"],
                (item.get("bindings") or []),
                bool(item.get("optional", False)))
    if isinstance(item, dict):
        return (item,
                (item.get("bindings") or []),
                bool(item.get("optional", False)))
    return item, [], False


def _resolve_to_rule_name(item: Any, rule_names: set[str]) -> str | None:
    if isinstance(item, str) and item in rule_names:
        return item
    if isinstance(item, dict):
        t = item.get("type")
        if isinstance(t, str) and t in rule_names:
            return t
    return None
