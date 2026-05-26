"""Worked example: parse → modify → save a JSON file with rawast.

This is the classic five-line use case that demonstrates the
bidirectional walk:

  1. Load a grammar (here, the bundled JSON grammar — same source the
     engine self-hosts from).
  2. Parse a file into a native Python value (dict / list / str / int /
     float / bool / None).
  3. Modify the parsed value with normal Python idioms.
  4. Save the modified value back through the same grammar.

The grammar drives both directions. No format-specific reader or
writer is needed; swap `Grammar("json")` for `Grammar("rawast")`,
`Grammar("gdsii")`, or any other bundled grammar to do the same dance
against a different format.
"""

from __future__ import annotations

import rawast


def main() -> None:
    g = rawast.Grammar("json")  # bundled grammar by short name

    # 1. Parse: native Python value out.
    text_in = '{"name": "alice", "items": [1, 2, 3], "ok": true}'
    value = g.parse_string(text_in)
    assert value == {"name": "alice", "items": [1, 2, 3], "ok": True}
    print("parsed   :", value)

    # 2. Modify: just Python.
    value["name"] = "bob"
    value["items"].append(4)
    value["new_field"] = None

    # 3. Save: same grammar, opposite direction. Result is bytes
    #    (binary grammars produce bytes too); decode for JSON.
    text_out = g.save(value, pretty=True).decode("utf-8")
    print("re-saved :", text_out)

    # 4. Round-trip: re-parse the output, confirm structural equality.
    value_again = g.parse_string(text_out)
    assert value_again == value
    print("round-trip ok.")


if __name__ == "__main__":
    main()
