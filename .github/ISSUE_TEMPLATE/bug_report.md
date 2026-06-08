---
name: Bug report
about: Report a crash, wrong output, or unexpected parser behaviour
title: '[bug] '
labels: bug
---

## What happened

<!-- One sentence: what went wrong. -->

## Reproduction

<!-- Minimal steps to reproduce. Ideally a small grammar + input
fragment plus the rawast command. -->

```sh
# command(s) that triggered the issue
```

**Grammar** (or filename of a bundled grammar, e.g. `lefdef.rawast`):

```
# grammar excerpt, or attach the file
```

**Input** (or a link to a public file, if larger):

```
# input excerpt
```

## Expected behaviour

<!-- What you thought should happen. -->

## Actual behaviour

<!-- What actually happened — full error message / stack trace if any.
For parse errors, the position info from the engine is most useful. -->

```
# paste the error output here
```

## Environment

- **rawast version / commit**: <!-- `rawast --version` or git rev-parse HEAD -->
- **OS**: <!-- e.g. Ubuntu 24.04, macOS 14.5, Windows 11 -->
- **Compiler**: <!-- gcc --version / clang --version -->
- **Python version** (if using the Python module): <!-- python --version -->
- **Install method**: <!-- pip install -e . / cmake build / wheel from PyPI -->

## Anything else

<!-- Logs, screenshots, related issues, or context that might help. -->
