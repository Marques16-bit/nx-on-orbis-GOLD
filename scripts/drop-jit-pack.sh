#!/usr/bin/env bash
# Takes the JIT pack (scripts/apply-jit-pack.sh) back out of the Eden checkout.
# Usage: bash scripts/drop-jit-pack.sh <Eden checkout> [--last]
#   (no option) the whole pack goes;
#   --last      only the last part goes (the direct table, part 2), when more than one part is in; part 1 (page reuse) stays.
# Exit 0: something was taken out; exit 1: there was nothing to take out.
set -euo pipefail
cd "$1"
if ! git rev-parse -q --verify refs/tags/nxo-jit-pack >/dev/null; then
    exit 1
fi
if [ "${2:-}" = "--last" ]; then
    count="$(git tag -l 'nxo-jit-pack-[0-9]*' | wc -l)"
    if [ "$count" -lt 2 ]; then
        exit 1
    fi
    git reset -q --hard "nxo-jit-pack-$((count - 1))"
    git tag -d "nxo-jit-pack-$count" >/dev/null
    git tag -f nxo-jit-pack "nxo-jit-pack-$((count - 1))" >/dev/null
    echo "JIT pack: the last part was taken out of the Eden checkout (part $count)"
    exit 0
fi
git reset -q --hard nxo-before-jit-pack
git tag -d nxo-jit-pack >/dev/null
for t in $(git tag -l 'nxo-jit-pack-[0-9]*'); do git tag -d "$t" >/dev/null; done
echo "JIT pack: taken out of the Eden checkout"
