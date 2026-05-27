---
name: Grammar proposal
about: Propose a new grammar for the community grammar repository
title: '[grammar] '
labels: grammar
---

## Format

<!-- Name of the file format you want to add a grammar for. -->

## Public specification

<!-- rawast only ships grammars for publicly-documented formats. Link
to the spec / standard document, or paste the relevant excerpt if the
spec is not online. -->

- Spec / standard:
- Spec version:

## Scope

<!-- Tier-1 structural readers are spec-faithful for file structure
but defer cross-cutting semantic constraints to the application
layer (the same scope discipline used by the bundled LEF/DEF/Tcl
grammars). Is your proposal Tier-1 structural, or are you proposing
something else? -->

- [ ] Tier-1 structural reader (parse-don't-validate)
- [ ] Tier-2 semantic schema (cross-reference resolution, range checks)
- [ ] Other (please describe)

## Test corpus

<!-- Grammars in the community repository land with a structural test
corpus. Do you have public files to validate against? -->

- Sample files (link):
- Approximate file size / count:
- License of the test files (must be openly distributable):

## Producer or consumer use case

<!-- Are you reading files of this format, writing them, or both? -->

- [ ] Consumer (reading existing files)
- [ ] Producer (writing files for other tools to consume)
- [ ] Both (parse + save round-trip)

## Engine primitives needed

<!-- If you've looked at the grammar surface and know the format needs
a specific terminal-parser group, sub-parse mechanism, or something
new, list it here. Otherwise skip this section. -->

## Anything else

<!-- Pointers to existing reference implementations (commercial or
open-source), known format quirks, or context that might help. -->
