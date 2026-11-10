#!/usr/bin/env bash
# Host test of Eden's src/common/orbis_lazy_memory.cpp. The file is not in this repository (Eden is fetched and patched
# at build time), so it is rebuilt here from the patches that create and change it, in order, and compiled with the
# console's kernel calls replaced by mmap (tests/lazy_memory_test.cpp). Run from the repository root.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
git init -q .
for p in "$ROOT"/patches/eden/*.patch; do
    if grep -q "orbis_lazy_memory" "$p"; then
        git apply --include='src/common/orbis_lazy_memory.*' "$p"
    fi
done
g++ -std=c++20 -Wall -Wextra -O1 -fsanitize=address,undefined -I"$WORK/src" \
    "$ROOT/tests/lazy_memory_test.cpp" "$WORK/src/common/orbis_lazy_memory.cpp" -o "$WORK/lazy_memory_test"
"$WORK/lazy_memory_test"
