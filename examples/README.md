# rawast examples

Small self-contained programs that exercise one rawast capability
end-to-end. Each example is runnable after `pip install -e .` from the
repository root.

| File | What it shows |
|---|---|
| [`json_edit.py`](json_edit.py) | Parse → modify → save round-trip on a JSON document using the bundled JSON grammar. The five-line "hello world" for the bidirectional walk. |

## Running

```sh
pip install -e .
python examples/json_edit.py
```

## Adding an example

Examples should fit on a single screen, focus on one capability, and
prefer the bundled grammars (`json`, `rawast`, `gdsii`, `lef`, `def`,
`tcl`) over user-supplied ones — that way the example runs with no
extra setup. Drop new examples in this directory and add a row to the
table above.
