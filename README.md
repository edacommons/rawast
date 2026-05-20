# rawast

A data-driven, predictive PEG parser engine for structured text and a
self-describing binary container format (`.jast`).

The grammar is a data file, not compiled code. Editing a `.json` grammar changes
parser behaviour with no recompilation. The engine produces a queryable in-memory
tree; the `.jast` container bundles the grammar with a compact serialisation so
that downstream consumers can read any file without prior knowledge of its
source format.

## Status

Pre-alpha. Phase 0 — project skeleton only. Not yet functional.

The previous prototype lives separately and is the design reference for this
rewrite. See the project's roadmap for what lands and when.

## Build

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires a C++20-capable compiler:

- GCC 11 or newer
- Clang 14 or newer
- Apple Clang 14 or newer

Dependencies (fetched automatically by CMake):

- [tl::expected](https://github.com/TartanLlama/expected) — error model, header-only.
- [doctest](https://github.com/doctest/doctest) — test framework, single-header, test-only.

## License

MIT — see [LICENSE](LICENSE).

## Author

Serge Rabyking · [LinkedIn](https://linkedin.com/in/serge-rabyking-b556ab89)
