# Security Policy

## Reporting a vulnerability

rawast is a parser engine — it routinely processes input from
potentially untrusted sources (third-party grammar files,
user-supplied input streams, `.jast` binary containers). Memory-safety
bugs, infinite-loop conditions, and unbounded-allocation paths in the
engine are in scope for security reports.

If you believe you have found a security vulnerability:

- **Please do not** open a public GitHub issue with the details.
- Email the maintainer at **s.rabykin@gmail.com** with the subject
  prefix `[rawast security]`. Include a minimal reproducer (grammar +
  input file) and a description of the impact.
- You will receive an acknowledgment within seven days. A coordinated
  fix and disclosure window will be agreed in the reply.

## Scope

In scope:

- Crashes, memory-safety bugs, or undefined behaviour in the C++ engine
  triggered by malformed grammars, malformed input streams, or
  malformed `.jast` files.
- Unbounded memory or CPU consumption (denial-of-service) reachable
  via grammars or inputs that the engine should reject in bounded
  time.
- Issues with the Python bindings that allow privilege escalation
  beyond what the calling Python code already has.

Out of scope:

- A grammar that is intentionally written to be ambiguous or
  pathological. Grammar correctness is the grammar author's
  responsibility; the engine surfaces lints via `rawast lint`.
- Issues in third-party dependencies (`tl::expected`, `doctest`,
  `nanobind`) — report those to their respective upstreams.

## Supported versions

Pre-1.0: only the latest commit on `main` is supported. Once 1.0
ships, this section will list supported branches.
