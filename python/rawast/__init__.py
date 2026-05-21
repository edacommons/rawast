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

Cross-format conversion is one line via a second grammar:

    json_g  = rawast.Grammar.load("grammars/json.json")
    gdsii_g = rawast.Grammar.load("grammars/gdsii.rawast")

    # Parse GDSII, emit JSON.
    text = json_g.save(gdsii_g.parse_file("layout.gds"))
"""

from ._native import Grammar, __version__

__all__ = ["Grammar", "__version__"]
