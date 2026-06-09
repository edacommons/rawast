# Building from source

Two build paths, depending on what you need.

## Python install from source (most users)

The standard `pip install rawast` path goes through scikit-build-core, which invokes CMake under the hood. You only need a working compiler + CMake; pip handles the rest.

```sh
python -m venv .venv && source .venv/bin/activate
pip install rawast
```

Compile takes ~15–20 seconds on a modern laptop. Once installed, `import rawast` works with zero runtime Python dependencies.

For development against the repo (editable install, runs from source):

```sh
git clone https://github.com/edacommons/rawast.git && cd rawast
python -m venv .venv && source .venv/bin/activate
pip install -e ".[test]"
```

`[test]` pulls `pytest`. Without it, you can still run the engine; you just can't run the Python test suite.

## C++ library and tests (contributors, C++ consumers)

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Produces a static library `librawast.a` and the doctest test runner. Both go under `build/`.

To install for `find_package(rawast)` consumers (planned for 1.0; the install rules are wired but not yet polished):

```sh
cmake --install build --prefix /your/prefix
```

## Compiler matrix

Requires C++17:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 7 | Wide-bar baseline. manylinux2014 ships gcc 9 by default. |
| Clang | 5 | |
| Apple Clang | 9 | Xcode 9.3+ |
| MSVC | 2017 (15.7+) | `/std:c++17` required |

Wide toolchain bar = wide install-from-source reach. The only C++20-era feature the engine briefly used was a defaulted spaceship operator on `NodeId`; expanded to six explicit comparisons under C++17 in commit `45d920c` so manylinux2014 wheels (CentOS 7 baseline) work without bumping the runtime requirement.

## CMake requirement

CMake 3.20 or newer. Most modern distros ship a compatible version. If you're on something old, install via your distro's CMake-backport channel or the official prebuilt binaries at https://cmake.org/download/.

## Dependencies

All dependencies are fetched automatically by CMake (or pip's build-isolation pass); none need to be installed manually.

| Dep | Purpose | License | When pulled |
|---|---|---|---|
| [tl::expected](https://github.com/TartanLlama/expected) | Error model, header-only | CC0 | Always |
| [doctest](https://github.com/doctest/doctest) | Test framework, single-header | MIT | C++ tests only |
| [nanobind](https://github.com/wjakob/nanobind) | Python binding generator | BSD-3-Clause | Python module build only |

rawast itself has **zero runtime Python dependencies** — `import rawast` works cleanly with no extra packages installed.

## Build configuration

CMake options on top of the standard ones:

| Option | Default | What it does |
|---|---|---|
| `RAWAST_BUILD_TESTS` | `ON` | Build the doctest test runner. Turn `OFF` for production library-only builds. |
| `RAWAST_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings as errors. The CI matrix runs with this `ON`. Useful for catching regressions before they land. |
| `CMAKE_BUILD_TYPE` | `Release` | Standard CMake build type. `Debug` enables `assert` and turns off `-O3 -DNDEBUG`. The Python wheel always builds `Release`. |

Example debug build with warnings-as-errors:

```sh
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug -DRAWAST_WARNINGS_AS_ERRORS=ON
cmake --build build-debug
ctest --test-dir build-debug --output-on-failure
```

## Cross-platform notes

| Platform | Tested in CI | Known issues |
|---|---|---|
| Linux x86_64 | ✓ (ubuntu-latest) | none |
| macOS arm64 | ✓ (macos-latest) | none |
| Linux aarch64 | not yet | should work; not yet exercised |
| macOS x86_64 | not yet | should work; not yet exercised |
| Windows | not yet | MSVC 2017+ should work in principle; not yet exercised |

The CI matrix covers `ubuntu-latest + macos-latest` × `Debug + Release` for C++, plus `ubuntu-latest + macos-latest` × `Python 3.10 + 3.12` for the Python tests. Windows support is a roadmap item (see [`ROADMAP.md`](ROADMAP.md)).

## Building the sdist (release engineering)

The sdist is what gets uploaded to PyPI and is also what end-users build from when no wheel matches.

```sh
python -m build --sdist
ls dist/   # rawast-X.Y.Z.tar.gz
```

The `[tool.scikit-build]` table in `pyproject.toml` configures the sdist contents (exclude rules trim `uv.lock`, `.github/`, and the dereferenced `python/rawast/grammars` symlink).

CI workflow at `.github/workflows/release.yml` builds and publishes the sdist to PyPI on a tag push (`v*`), via OIDC trusted publishing.

## See also

- [`FEATURES.md`](FEATURES.md) — what you get after building
- [`CLI.md`](CLI.md) — running the binary
- [`ROADMAP.md`](ROADMAP.md) — including cibuildwheel plans for pre-built wheels
- [`CONTRIBUTING.md`](../CONTRIBUTING.md) — how to submit changes
