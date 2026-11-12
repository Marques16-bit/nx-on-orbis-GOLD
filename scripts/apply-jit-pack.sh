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
git tag -d nxo-jit-pack-1 >/dev/null 2>&1 || true
err="$(mktemp)"
applied=0
# The parts in file-name order: *.patch (git am) and *.py (anchored edits for a file that is not at hand when they are written; see the
# script itself). A part that does not fit is skipped with a warning and so is everything after it; the parts before it stay, because
# part 1 (page reuse) is useful without part 2 (direct table), and part 2 needs part 1.
for p in "$JITDIR"/*.patch "$JITDIR"/*.py; do
    [ -e "$p" ] || continue
    printf '%s\n' "$p"
done | sort | while IFS= read -r p; do
    fits=0
    case "$p" in
    *.patch)
        if git apply --check "$p" 2>"$err"; then
            git -c user.name=nx-on-orbis -c user.email=nx-on-orbis@localhost am -q --keep-cr "$p"
            fits=1
        fi
        ;;
    *.py)
        if python3 -I "$p" . 2>"$err" >/dev/null; then
            git add -A
            git -c user.name=nx-on-orbis -c user.email=nx-on-orbis@localhost commit -q -m "JIT pack: $(basename "$p" .py)"
            fits=1
        else
            git checkout -q -- . 2>/dev/null || true
        fi
        ;;
    esac
    if [ "$fits" = 1 ]; then
        applied=$((applied + 1))
        git tag -f "nxo-jit-pack-$applied" >/dev/null
        echo "JIT pack: applied $(basename "$p")"
    else
        echo "::warning::JIT pack: $(basename "$p") does not fit this Eden commit; the build goes on without it and without any part after it"
        sed 's/^/    /' "$err" | head -n 12
        break
    fi
done
rm -f "$err"
if git rev-parse -q --verify refs/tags/nxo-jit-pack-1 >/dev/null; then
    last="$(git tag -l 'nxo-jit-pack-[0-9]*' | sort -t- -k4 -n | tail -n 1)"
    git tag -f nxo-jit-pack "$last" >/dev/null
fi
exit 0
