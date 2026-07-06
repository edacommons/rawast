#!/usr/bin/env bash
# End-to-end proof that rawast is consumable as a generic engine: build +
# install rawast, then build THIS example against the installed package via
# find_package(rawast) and run it. Exits non-zero if any step fails or the
# custom representation does not round-trip identically to the reference.
#
# Usage: examples/custom-representation/verify.sh   (run from the repo root)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "== build + install rawast =="
cmake -S "$repo" -B "$work/build" -DRAWAST_BUILD_TESTS=OFF >/dev/null
cmake --build "$work/build" -j >/dev/null
cmake --install "$work/build" --prefix "$work/prefix" >/dev/null

echo "== build example via find_package(rawast) =="
cmake -S "$here" -B "$work/ex" -DCMAKE_PREFIX_PATH="$work/prefix" >/dev/null
cmake --build "$work/ex" -j >/dev/null

echo "== run =="
printf '{"list":[1,2,3],"n":42,"name":"alice","ok":true}' > "$work/in.json"
out="$("$work/ex/custom-representation" "$repo/grammars/json.json" "$work/in.json")"
echo "$out"
echo "$out" | grep -q "custom-save == reference-save : yes" \
    || { echo "FAIL: custom representation did not match reference"; exit 1; }
echo "OK: rawast consumed via find_package; custom representation round-trips."
