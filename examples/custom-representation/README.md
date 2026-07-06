# Building your own value representation on the rawast seam

rawast is a **generic, representation-agnostic engine**. It never depends on a
concrete value type — it speaks exactly two interfaces, and a *representation*
is a `{Builder, Accessor}` pair:

| interface | half  | header |
|-----------|-------|--------|
| `Builder`  | write — the parser emits typed events into it | `<rawast/builder.hpp>` |
| `Accessor` | read  — the save engine / `convert` read through it | `<rawast/accessor.hpp>` |

Implement both and every engine operation works on your representation for free:

```cpp
grammar.parse_into(stream, my_builder);      // parse INTO your representation
grammar.save_from(out, my_accessor);         // serialise FROM it
convert(my_accessor, other_builder);         // pipe it into any other one
```

Concrete representations — arenas, columnar stores, host-native structures —
belong in the **consuming project**, not in the rawast core. This example is
one such representation, kept deliberately small.

## What this example is

A **compact, interned** representation: all nodes live in one flat `std::vector`,
and every dict key and string value is interned into a shared pool (in real
ASTs, keys like `type`/`name` and tags like `logic` repeat by the million).
It's the shape a memory-tight consumer would build. `main.cpp` implements the
`{Builder, Accessor}` pair against **only public rawast headers** and proves it
round-trips: parse an input into the compact store, `save_from` it, and check
the bytes match rawast's own reference representation.

## Consume rawast via find_package

```bash
# 1. Install rawast somewhere (from the rawast source tree):
cmake -S <rawast-src> -B /tmp/rawast-build
cmake --build /tmp/rawast-build
cmake --install /tmp/rawast-build --prefix /tmp/rawast-prefix

# 2. Build this example against the installed rawast:
cmake -S . -B build -DCMAKE_PREFIX_PATH=/tmp/rawast-prefix
cmake --build build

# 3. Run it: <grammar> <input>
./build/custom-representation <rawast-src>/grammars/json.json input.json
# -> custom-save == reference-save : yes
```

`verify.sh` runs the whole cycle end to end.

## Notes

* The `Builder` uses the standard "buffer children per open level, pair dict
  keys when the level closes" pattern, so PEG backtracking
  (`checkpoint`/`rollback`/`record_from`/`replay`) is trivially correct and
  keys that arrive through spliced `None` sequences still land on the right
  dict.
* `Accessor::each` yields keys **sorted** — the contract the save engine and
  round-trip equality rely on.
* `adopt(ValuePtr)` is not overridden: the base class default translates a
  reference-model subtree into typed events, so a plug-in gets it for free.
