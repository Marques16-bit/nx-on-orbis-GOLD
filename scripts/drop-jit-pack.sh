#!/usr/bin/env bash
# Takes the JIT pack (scripts/apply-jit-pack.sh) back out of the Eden checkout. Usage: bash scripts/drop-jit-pack.sh <Eden checkout>
# Exit 0: the pack was in and is gone now; exit 1: there was no pack to take out.
set -euo pipefail
cd "$1"
if ! git rev-parse -q --verify refs/tags/nxo-jit-pack >/dev/null; then
    exit 1
fi
git reset -q --hard nxo-before-jit-pack
git tag -d nxo-jit-pack >/dev/null
echo "JIT pack: taken out of the Eden checkout"
