"""Command-line interface for rawast.

Installed as the `rawast` console script (see pyproject.toml). Also
runnable as `python -m rawast`.
"""

from __future__ import annotations

import argparse
import json
import sys

from . import Grammar, __version__


def cmd_parse(args: argparse.Namespace) -> int:
    g = Grammar.load(args.grammar)
    profile_top = getattr(args, "profile_top", None)
    profile_on = getattr(args, "profile", False) or profile_top is not None
    if profile_on:
        g.profile_enable(True)
    ast = g.parse_file(args.input)
    if profile_on:
        # Emit the AST silently when profiling — the report goes to
        # stderr below, the AST still goes to stdout so pipelines
        # like `rawast parse --profile g f > ast.json` still work.
        pass
    indent = 2 if args.pretty else None
    json.dump(ast, sys.stdout, indent=indent, ensure_ascii=False)
    sys.stdout.write("\n")
    if profile_on:
        _print_profile_report(g.last_profile_report(), profile_top)
    return 0


def _print_profile_report(report, top_arg) -> None:
    """Pretty-print a profile report to stderr.

    `top_arg` is the value of `--profile-top`: None (default top-20),
    the string "all" (every rule), or a positive integer N (top-N).
    """
    n = 20
    if isinstance(top_arg, str):
        if top_arg == "all":
            n = 0  # 0 means "no truncation"
        else:
            try:
                n = max(1, int(top_arg))
            except ValueError:
                n = 20
    entries = sorted(report["entries"],
                     key=lambda e: e["total_ns"], reverse=True)
    if n > 0:
        entries = entries[:n]
    # Filter to entries that have a rule name (anonymous inline nodes
    # are usually noise — when you need them, --profile-top=all also
    # disables the name filter).
    show_anonymous = (n == 0)
    if not show_anonymous:
        entries = [e for e in entries if e["rule_name"]]

    total_ns = report["total_ns"] or 1   # avoid div-by-zero
    print("", file=sys.stderr)
    print(f"profile: {report['total_ns'] / 1e6:.2f}ms total, "
          f"{report['total_frames']} frames, max depth {report['max_depth']}",
          file=sys.stderr)
    header = f"  {'rule':<35} {'calls':>9} {'fails':>9} {'ns':>14} {'pct':>5} {'depth':>6}"
    print(header, file=sys.stderr)
    print("  " + "-" * (len(header) - 2), file=sys.stderr)
    for e in entries:
        name = e["rule_name"] or f"<inline#{e['node_id']}>"
        if len(name) > 35:
            name = name[:32] + "..."
        pct = 100.0 * e["total_ns"] / total_ns
        print(f"  {name:<35} "
              f"{e['entry_count']:>9} "
              f"{e['fail_count']:>9} "
              f"{e['total_ns']:>14} "
              f"{pct:>4.1f}% "
              f"{e['max_depth']:>6}",
              file=sys.stderr)


def cmd_save(args: argparse.Namespace) -> int:
    g = Grammar.load(args.grammar)
    with open(args.input) as f:
        value = json.load(f)
    data = g.save(value, pretty=args.pretty)
    sys.stdout.buffer.write(data)
    return 0


def cmd_convert(args: argparse.Namespace) -> int:
    src = Grammar.load(args.read)
    dst = Grammar.load(args.write)
    ast = src.parse_file(args.input)
    sys.stdout.buffer.write(dst.save(ast, pretty=args.pretty))
    return 0


def cmd_profile(args: argparse.Namespace) -> int:
    """Profile parsing across a list of files; print an aggregated
    per-rule report.

    Aggregates entry_count / fail_count / total_ns per rule_name
    across every input; max_depth is the deepest seen on any one
    file. Per-file timings still measure single-call wall-clock,
    so the total here is the sum of per-call totals (which matches
    "wall-clock time spent inside parse_from across the corpus" —
    serial; this CLI doesn't fan out).
    """
    from pathlib import Path
    files = []
    for arg in args.inputs:
        p = Path(arg)
        if p.is_dir():
            for ext in args.ext.split(","):
                files.extend(sorted(p.rglob(f"*.{ext.strip()}")))
        else:
            files.append(p)
    if getattr(args, "from_file", None):
        with open(args.from_file) as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                files.append(Path(line))
    if not files:
        print("profile: no input files", file=sys.stderr)
        return 1

    g = Grammar.load(args.grammar)
    g.profile_enable(True)

    # Aggregate: rule_name -> {entry_count, fail_count, total_ns, max_depth}.
    agg: dict[str, dict[str, int]] = {}
    grand_total_ns = 0
    grand_total_frames = 0
    grand_max_depth = 0
    file_failures: list[tuple[str, str]] = []

    for f in files:
        try:
            g.parse_file(str(f))
        except Exception as e:
            file_failures.append((str(f), str(e)[:120]))
            continue
        rep = g.last_profile_report()
        grand_total_ns += rep["total_ns"]
        grand_total_frames += rep["total_frames"]
        if rep["max_depth"] > grand_max_depth:
            grand_max_depth = rep["max_depth"]
        for e in rep["entries"]:
            name = e["rule_name"] or f"<inline#{e['node_id']}>"
            slot = agg.setdefault(name, {
                "entry_count": 0,
                "fail_count":  0,
                "total_ns":    0,
                "max_depth":   0,
            })
            slot["entry_count"] += e["entry_count"]
            slot["fail_count"]  += e["fail_count"]
            slot["total_ns"]    += e["total_ns"]
            if e["max_depth"] > slot["max_depth"]:
                slot["max_depth"] = e["max_depth"]

    # Skip anonymous unless --top is "all".
    show_anonymous = (args.top == "all")
    items = [(name, slot) for name, slot in agg.items()
             if show_anonymous or not name.startswith("<inline#")]

    sort_key = {
        "time":  lambda s: s[1]["total_ns"],
        "count": lambda s: s[1]["entry_count"],
        "fails": lambda s: s[1]["fail_count"],
    }[args.by]
    items.sort(key=sort_key, reverse=True)

    n = 20
    if args.top == "all":
        n = 0
    elif args.top is not None:
        try:
            n = max(1, int(args.top))
        except ValueError:
            n = 20
    if n > 0:
        items = items[:n]

    grand_total_ns = grand_total_ns or 1
    print("", file=sys.stderr)
    print(f"corpus profile: {len(files)} files, "
          f"{grand_total_ns / 1e6:.2f}ms total parse, "
          f"{grand_total_frames} frames, max depth {grand_max_depth}, "
          f"sorted by {args.by}",
          file=sys.stderr)
    if file_failures:
        print(f"  ({len(file_failures)} file(s) failed to parse; first: "
              f"{file_failures[0][0]} — {file_failures[0][1]})",
              file=sys.stderr)
    header = (f"  {'rule':<35} {'calls':>9} {'fails':>9} "
              f"{'ns':>14} {'pct':>5} {'depth':>6}")
    print(header, file=sys.stderr)
    print("  " + "-" * (len(header) - 2), file=sys.stderr)
    for name, slot in items:
        disp = name if len(name) <= 35 else name[:32] + "..."
        pct = 100.0 * slot["total_ns"] / grand_total_ns
        print(f"  {disp:<35} "
              f"{slot['entry_count']:>9} "
              f"{slot['fail_count']:>9} "
              f"{slot['total_ns']:>14} "
              f"{pct:>4.1f}% "
              f"{slot['max_depth']:>6}",
              file=sys.stderr)
    return 0


def cmd_lint(args: argparse.Namespace) -> int:
    g = Grammar.load(args.grammar)
    issues = g.lint()
    if not issues:
        print("OK")
        return 0
    for issue in issues:
        print(f"warning: {issue}", file=sys.stderr)
    return 1


def cmd_docs(args: argparse.Namespace) -> int:
    from .docs import to_markdown
    meta = Grammar() if args.grammar.endswith(".json") else Grammar("rawast")
    grammar = meta.parse_file(args.grammar)
    title = args.title or args.grammar
    sys.stdout.write(to_markdown(grammar, title=title,
                                 heading_level=args.heading_level))
    sys.stdout.write("\n")
    return 0


def cmd_schema(args: argparse.Namespace) -> int:
    from .schema import to_markdown
    meta = Grammar() if args.grammar.endswith(".json") else Grammar("rawast")
    grammar = meta.parse_file(args.grammar)
    title = args.title or args.grammar
    sys.stdout.write(to_markdown(grammar, title=title,
                                 heading_level=args.heading_level))
    sys.stdout.write("\n")
    return 0


def cmd_pydantic(args: argparse.Namespace) -> int:
    from .pydantic_gen import to_pydantic
    meta = Grammar() if args.grammar.endswith(".json") else Grammar("rawast")
    grammar = meta.parse_file(args.grammar)
    doc = args.module_doc or f"Pydantic models generated from {args.grammar}."
    sys.stdout.write(to_pydantic(grammar, module_doc=doc))
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="rawast",
        description="Data-driven predictive PEG parser engine. "
                    "One grammar drives parse, save, validate.",
    )
    parser.add_argument("--version", action="version", version=f"rawast {__version__}")
    sub = parser.add_subparsers(dest="cmd", required=True, metavar="COMMAND")

    p_parse = sub.add_parser("parse", help="Parse an input file; emit AST as JSON")
    p_parse.add_argument("grammar", help="Grammar file (.rawast or .json)")
    p_parse.add_argument("input", help="Input file to parse")
    p_parse.add_argument("--pretty", action="store_true",
                         help="Pretty-print the JSON output (indent 2)")
    p_parse.add_argument("--profile", action="store_true",
                         help="Print a per-rule profile (entry count, fail "
                              "count, total time, %% of parse, max depth) "
                              "to stderr after the AST. Default top 20.")
    p_parse.add_argument("--profile-top", metavar="N",
                         help="Like --profile but print top-N rules instead "
                              "of the default 20. Pass `all` to print every "
                              "rule including anonymous (inline) nodes.")
    p_parse.set_defaults(func=cmd_parse)

    p_save = sub.add_parser("save", help="Save a JSON-described value as the grammar's format")
    p_save.add_argument("grammar", help="Grammar file")
    p_save.add_argument("input", help="JSON file containing the value to serialise")
    p_save.add_argument("--pretty", action="store_true",
                        help="Use the grammar's pretty-print formatting")
    p_save.set_defaults(func=cmd_save)

    p_convert = sub.add_parser("convert",
                                help="Parse with one grammar, save with another")
    p_convert.add_argument("input", help="Input file")
    p_convert.add_argument("--read", required=True, help="Grammar to parse with")
    p_convert.add_argument("--write", required=True, help="Grammar to save with")
    p_convert.add_argument("--pretty", action="store_true")
    p_convert.set_defaults(func=cmd_convert)

    p_lint = sub.add_parser("lint", help="Lint a grammar")
    p_lint.add_argument("grammar", help="Grammar file")
    p_lint.set_defaults(func=cmd_lint)

    p_profile = sub.add_parser("profile",
        help="Parse a list of files (or directories), aggregate per-rule "
             "entry / fail / time counters, print sorted top-N to stderr")
    p_profile.add_argument("grammar", help="Grammar file (.rawast or .json)")
    p_profile.add_argument("inputs", nargs="*",
                           help="Zero or more input files / directories. "
                                "Directories are recursed for files matching "
                                "the --ext extensions (default: lef,tlef,def).")
    p_profile.add_argument("--from-file", metavar="LIST",
                           help="Read additional input paths from a file, "
                                "one per line. Blank lines and `#` comments "
                                "are skipped.")
    p_profile.add_argument("--ext", default="lef,tlef,def",
                           help="Comma-separated extensions to recurse into "
                                "directories. Default: lef,tlef,def.")
    p_profile.add_argument("--top", metavar="N",
                           help="Rows to print (default 20). Pass `all` to "
                                "print every rule including anonymous "
                                "(inline) nodes.")
    p_profile.add_argument("--by", choices=("time", "count", "fails"),
                           default="time",
                           help="Sort key: total wall-clock time (default), "
                                "entry count, or failure count.")
    p_profile.set_defaults(func=cmd_profile)

    p_docs = sub.add_parser("docs", help="Render a grammar as EBNF-flavoured Markdown")
    p_docs.add_argument("grammar", help="Grammar file (.rawast or .json)")
    p_docs.add_argument("--title", help="Document title (defaults to the grammar path)")
    p_docs.add_argument("--heading-level", type=int, default=1,
                        help="Markdown heading level for the title (default 1). "
                             "Per-rule headings are one level deeper. Bump to "
                             "nest the output inside an existing section.")
    p_docs.set_defaults(func=cmd_docs)

    p_schema = sub.add_parser(
        "schema",
        help="Render a grammar's value-tree shape as Markdown (dict fields, "
             "array elements, choice alternatives — the shape a producer "
             "tool must build to call `save()`)",
    )
    p_schema.add_argument("grammar", help="Grammar file (.rawast or .json)")
    p_schema.add_argument("--title", help="Document title (defaults to the grammar path)")
    p_schema.add_argument("--heading-level", type=int, default=1,
                          help="Markdown heading level for the title (default 1). "
                               "Per-rule headings are one level deeper.")
    p_schema.set_defaults(func=cmd_schema)

    p_pydantic = sub.add_parser(
        "pydantic",
        help="Generate Pydantic v2 model source for a grammar — emits a "
             "Python module whose models mirror the grammar's parse/save "
             "dict shape exactly (round-trip: parse → validate → dump "
             "yields the same dict). Output goes to stdout; redirect "
             "to a file.",
    )
    p_pydantic.add_argument("grammar", help="Grammar file (.rawast or .json)")
    p_pydantic.add_argument("--module-doc",
                            help="Docstring for the generated module")
    p_pydantic.set_defaults(func=cmd_pydantic)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
