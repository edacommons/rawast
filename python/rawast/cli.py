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
    ast = g.parse_file(args.input)
    indent = 2 if args.pretty else None
    json.dump(ast, sys.stdout, indent=indent, ensure_ascii=False)
    sys.stdout.write("\n")
    return 0


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
