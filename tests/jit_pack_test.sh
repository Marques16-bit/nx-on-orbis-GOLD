#!/usr/bin/env bash
# Host test of the JIT page reuse pack's IR pass: cuts the pass out of patches/eden-jit/*.patch, compiles it against a stand-in
# of Dynarmic's IR (tests/jit_pack/) and runs hand-built blocks through it. Run from the repository root:
#   bash tests/jit_pack_test.sh
set -euo pipefail
cd "$(dirname "$0")/.."
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
patch_file="$(ls patches/eden-jit/*.patch | head -n 1)"
# The pass sits between two marker comments of opt_passes.cpp's hunk; strip the diff's leading '+'.
awk '/^\+\/\/ ---- nx-on-orbis: one page lookup/ {on=1} on {print substr($0, 2)} /^\+\/\/ ---- end of nx-on-orbis page reuse/ {on=0}' "$patch_file" >"$WORK/pass.inc"
lines="$(wc -l <"$WORK/pass.inc")"
echo "pass cut out of $(basename "$patch_file"): $lines lines"
[ "$lines" -gt 100 ] || { echo "FAIL: the pass was not found in the patch"; exit 1; }
g++ -std=c++20 -Wall -Wextra -Wno-unused-function -DPASS_INC="\"$WORK/pass.inc\"" -Itests/jit_pack tests/jit_pack/pass_test.cpp -o "$WORK/pass_test"
"$WORK/pass_test"
