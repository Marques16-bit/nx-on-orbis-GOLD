#!/usr/bin/env bash
# The JIT pack (patches/eden-jit/): optional patches to Eden's Dynarmic, applied on top of patches/eden/ by
# scripts/fetch-eden.sh. Usage: bash scripts/apply-jit-pack.sh <repo root> <Eden checkout>
#
# Unlike patches/eden/ (one that does not apply breaks the whole build), the pack is allowed to not fit:
#   - a pack patch that does not apply to this Eden commit is skipped with a warning (the tree is left exactly
#     as it was) and the build goes on without the pack;
#   - ci/stage-eden.sh takes the pack out again (scripts/drop-jit-pack.sh) if the build then fails inside Dynarmic.
# patches/eden-jit/MODE holds "on" (default) or "off"; the environment variable NXO_JIT_PACK overrides it.
# Either way the console's boot.log says whether the build has the pack ("JIT page reuse: ...").
set -euo pipefail
ROOT="$1"
DEST="$2"
JITDIR="$ROOT/patches/eden-jit"

mode="${NXO_JIT_PACK:-}"
if [ -z "$mode" ] && [ -f "$JITDIR/MODE" ]; then
    mode="$(tr -d ' \t\r\n' < "$JITDIR/MODE")"
fi
mode="${mode:-on}"
if [ "$mode" != "on" ]; then
    echo "JIT pack: $mode (patches/eden-jit/MODE or NXO_JIT_PACK): not applied"
    exit 0
fi

cd "$DEST"
git tag -f nxo-before-jit-pack >/dev/null
err="$(mktemp)"
applied=0
for p in "$JITDIR"/*.patch; do
    [ -e "$p" ] || continue
    if git apply --check "$p" 2>"$err"; then
        git -c user.name=nx-on-orbis -c user.email=nx-on-orbis@localhost am -q --keep-cr "$p"
        applied=$((applied + 1))
        echo "JIT pack: applied $(basename "$p")"
    else
        echo "::warning::JIT pack: $(basename "$p") does not fit this Eden commit; the build goes on without the pack"
        sed 's/^/    /' "$err" | head -n 12
        git reset -q --hard nxo-before-jit-pack
        applied=0
        break
    fi
done
rm -f "$err"
if [ "$applied" -gt 0 ]; then
    git tag -f nxo-jit-pack >/dev/null
fi
exit 0
