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

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
