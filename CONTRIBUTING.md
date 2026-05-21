# Contributing to rawast

Thanks for the interest. rawast is small enough that any thoughtful
issue or pull request is welcome.

## Getting set up

```sh
git clone https://github.com/edacommons/rawast.git
cd rawast

# C++ build + tests
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure

# Python build + tests
python -m venv .venv && source .venv/bin/activate
pip install -e ".[test]"
pytest python/tests/
```

The C++ tests use [doctest](https://github.com/doctest/doctest) and
the Python tests use [pytest](https://pytest.org). Both run on every
push via the CI workflow in `.github/workflows/ci.yml`.

## What's helpful

- **Bug reports.** Open an issue with a minimal reproduction —
  ideally a small grammar fragment and the input that misbehaves.
- **New grammars.** Each grammar (`grammars/*.rawast`) is mostly
  self-contained data; a new format becomes a new file plus
  whatever terminal parsers it needs. See `grammars/json.json` and
  `grammars/gdsii.rawast` for examples of text and binary patterns
  respectively, and `docs/rawast-format.md` for the language spec.
- **Pull requests.** For non-trivial changes, opening an issue first
  to talk through the approach saves time. Small fixes are fine to
  submit directly.

## Style

- C++20, `clang-format`-friendly two-space indent, no tabs.
- Public APIs in `include/rawast/`, implementations in `src/`.
- Test additions go in `tests/` (doctest) or `python/tests/` (pytest).
- Commit messages: imperative subject (`feat:`, `fix:`, `docs:`,
  `refactor:`, `test:`), wrapped at ~72 columns, with body
  explaining *why* the change exists rather than restating the diff.

## License

By contributing you agree your changes are released under the
project's MIT license (see [LICENSE](LICENSE)).
