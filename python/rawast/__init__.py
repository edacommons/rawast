"""rawast — data-driven predictive PEG parser engine.

One grammar file drives parsing, saving, and validation. Supports
both structured text formats (JSON, LEF, DEF, Liberty, SPEF, Verilog
netlist, …) and binary formats (GDSII).

Basic usage:

    import rawast

    g = rawast.Grammar.load("grammars/json.json")

    ast = g.parse_string('{"name": "alice", "items": [1, 2, 3]}')
    # ast == {"name": "alice", "items": [1, 2, 3]}

    text = g.save(ast)                     # returns bytes
    print(text.decode("utf-8"))             # JSON text

    issues = g.lint()                      # ambiguous-Choice warnings

Browse a grammar as data (introspection / Pydantic generation /
schema export):

    rawast_meta = rawast.rawast_format()
    grammar_dict = rawast_meta.parse_file("grammars/gdsii.rawast")
    # grammar_dict["LIBRARY"]["type"] == "sequence"
    # grammar_dict["LIBRARY"]["items"][0] ==
    #     {"expr": {"type": "gds_header"}, "name": "version",
    #      "type": "binding"}

Construct or transform grammars programmatically:

    grammar_dict["NEW_RULE"] = {"type": "sequence", ...}
    new_grammar = rawast.Grammar.from_dict(grammar_dict)
    ast = new_grammar.parse_file("input.txt")

Cross-format conversion is one line via a second grammar:

    json_g  = rawast.Grammar.load("grammars/json.json")
    gdsii_g = rawast.Grammar.load("grammars/gdsii.rawast")
    text    = json_g.save(gdsii_g.parse_file("layout.gds"))
"""

from __future__ import annotations

import os

from ._native import Grammar, __version__


# Path to bundled grammar files. The wheel ships these under
# rawast/grammars/; in editable installs the symlink at
# python/rawast/grammars → ../../grammars resolves to the repo's
# grammars/ directory.
GRAMMAR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "grammars")


def grammar_path(name: str) -> str:
    """Return the absolute path of a bundled grammar file by name.

    >>> grammar_path("json.json")
    '/.../site-packages/rawast/grammars/json.json'
    """
    return os.path.join(GRAMMAR_DIR, name)


def json_format() -> Grammar:
    """The JSON grammar.

    Parses any JSON document, including JSON-form grammar files. Use
    this to browse a JSON-form grammar as a Python dict::

        g = rawast.json_format()
        grammar_dict = g.parse_file("grammars/lef.json")
    """
    return Grammar.json_format_builtin()


def rawast_format() -> Grammar:
    """The `.rawast` meta-grammar.

    Parses `.rawast` source text into the same dict shape that
    `json_format()` produces for JSON-form grammar files. Use this to
    browse a `.rawast` file as data::

        g = rawast.rawast_format()
        grammar_dict = g.parse_file("grammars/gdsii.rawast")
    """
    return Grammar.load(grammar_path("rawast.json"))


__all__ = [
    "Grammar",
    "GRAMMAR_DIR",
    "grammar_path",
    "json_format",
    "rawast_format",
    "__version__",
]
