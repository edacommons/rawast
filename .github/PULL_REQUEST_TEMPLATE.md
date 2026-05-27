<!-- Thanks for contributing to rawast. Please keep PRs focused on one
change; large unrelated diffs are easier to review when split. -->

## What this changes

<!-- One or two sentences describing the change. -->

## Why

<!-- The motivation: what problem does this solve, or what capability
does it add? Link a related issue if one exists. -->

Closes #<!-- issue number, if any -->

## Type of change

- [ ] Bug fix (non-breaking change that fixes incorrect behaviour)
- [ ] New feature (engine, CLI, API, or grammar addition)
- [ ] Refactor (no observable behaviour change)
- [ ] Documentation only
- [ ] Build / CI / tooling
- [ ] Other (please describe)

## Checklist

- [ ] Tests added or updated (`tests/` for C++, `python/tests/` for
      Python).
- [ ] `ctest --test-dir build --output-on-failure` passes locally.
- [ ] `pytest python/tests/` passes locally.
- [ ] If you touched a grammar, `rawast lint <grammar>` is clean.
- [ ] If you added a new public API, `docs/rawast-format.md` is updated.
- [ ] Commit messages use imperative subject (`feat:`, `fix:`,
      `docs:`, `refactor:`, etc.), wrap at ~72 columns, and explain
      *why* in the body when the diff isn't obvious.
- [ ] No local-only files committed (proposal drafts, personal notes,
      `.venv/`, `.claude/`, etc.).

## Notes for reviewers

<!-- Anything you'd like a reviewer to focus on, alternative
approaches you considered, or trade-offs you made. Optional. -->
