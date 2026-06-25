#!/usr/bin/env python3
"""Format-diff harness: compare rawast's SystemVerilog pretty-print against
`verible-verilog-format` (Google Verible — the lowRISC house-style formatter)
on the same source.

It is a *style oracle*, not a correctness check: rawast's save round-trip is
verified elsewhere. This tool surfaces where rawast's whitespace/layout
diverges from a real-world reference so the grammar's pretty-print attrs can
be tuned (or a divergence consciously accepted as a style choice).

Usage:
    python tools/verible_format_diff.py [--summary] FILE.sv [FILE.sv ...]

verible binary resolution (first hit wins):
    $VERIBLE_FORMAT env var, then `verible-verilog-format` on PATH.
Download a build for your platform from
https://github.com/chipsalliance/verible/releases (Intel macOS needs an
older x86_64 build; recent macOS assets are arm64-only).
"""
from __future__ import annotations

import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys

import rawast


def find_verible() -> str | None:
    return os.environ.get("VERIBLE_FORMAT") or shutil.which("verible-verilog-format")


def verible_fmt(verible: str, path: str) -> str | None:
    r = subprocess.run([verible, path], capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def rawast_fmt(sv: "rawast.Grammar", src: str) -> str:
    out = sv.save(sv.parse_string(src))
    return out.decode("utf-8") if isinstance(out, (bytes, bytearray)) else out


# Heuristic categories for a (verible-line, rawast-line) divergence — purely
# to summarize *kinds* of difference across a corpus, not to be exhaustive.
def categorize(vline: str, rline: str) -> str:
    v, r = vline.strip(), rline.strip()
    if v.replace(" ", "") == r.replace(" ", ""):
        # same tokens, different spacing
        if "#(" in r or ")(" in r:
            return "tight #( / )( (rawast omits space)"
        if "  " in r and "  " not in v:
            return "double-space (rawast)"
        if re.search(r"\[\w+ [-+*/] ", r) and re.search(r"\[\w+[-+*/]", v):
            return "spaced range [a - 1] vs tight [a-1]"
        return "spacing (other)"
    vlead = len(vline) - len(vline.lstrip())
    rlead = len(rline) - len(rline.lstrip())
    if v == r and vlead != rlead:
        return f"indent width ({rlead}->{vlead})"
    if v.replace(" ", "") == r.replace(" ", "") + "":
        return "alignment"
    return "structural / other"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--summary", action="store_true",
                    help="aggregate difference categories across all files")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    verible = find_verible()
    if not verible:
        print("verible-verilog-format not found "
              "(set $VERIBLE_FORMAT or put it on PATH)", file=sys.stderr)
        return 2

    sv = rawast.Grammar("systemverilog")
    cats: dict[str, int] = {}
    identical = diffed = failed = 0

    for path in args.files:
        try:
            src = open(path).read()
        except OSError as e:
            print(f"{path}: cannot read ({e})"); failed += 1; continue
        v = verible_fmt(verible, path)
        try:
            r = rawast_fmt(sv, src)
        except Exception as e:  # noqa: BLE001
            print(f"{path}: rawast parse/save failed: {str(e)[:70]}")
            failed += 1
            continue
        if v is None:
            print(f"{path}: verible format failed"); failed += 1; continue

        vl, rl = v.splitlines(), r.splitlines()
        if vl == rl:
            identical += 1
            if not args.summary:
                print(f"=== {path} ===\n  IDENTICAL")
            continue
        diffed += 1
        if args.summary:
            sm = difflib.SequenceMatcher(None, vl, rl)
            for tag, i1, i2, j1, j2 in sm.get_opcodes():
                if tag == "equal":
                    continue
                for vk, rk in zip(vl[i1:i2], rl[j1:j2]):
                    cats[categorize(vk, rk)] = cats.get(categorize(vk, rk), 0) + 1
        else:
            print(f"=== {path} ===")
            print("\n".join(difflib.unified_diff(
                vl, rl, "verible", "rawast", lineterm="")))

    print(f"\nfiles: identical={identical} diff={diffed} failed={failed}")
    if args.summary and cats:
        print("difference categories (line pairs):")
        for c, n in sorted(cats.items(), key=lambda kv: -kv[1]):
            print(f"  {n:4}  {c}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
