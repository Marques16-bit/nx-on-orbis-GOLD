#!/usr/bin/env bash
# Host test of scripts/apply-jit-pack.sh and scripts/drop-jit-pack.sh with the staged JIT pack (part 1 a patch, part 2 an anchored-edit script):
# parts apply in order, a part that does not fit stops the ones after it but keeps the ones before, and drop takes out the last part or all.
# The real part 2 script runs on the Eden fixtures; part 1 is stood in by a one-line patch. Run from the repository root.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
D=src/dynarmic/src/dynarmic/backend/x64
FX="$ROOT/tests/jit_pack/fixtures"
pass=0
ok() { pass=$((pass + 1)); }
fail() { echo "FAIL: $*"; exit 1; }
G=(git -c user.name=t -c user.email=t@t)

newcase() { # $1 = name; makes $WORK/$1/{repo,eden}
    local c="$WORK/$1"
    mkdir -p "$c/repo/patches/eden-jit" "$c/eden/src/common" "$c/eden/src/core" "$c/eden/$D"
    cp "$ROOT/patches/eden-jit/0002-JIT-direct-page-table-copy.py" "$c/repo/patches/eden-jit/"
    echo on >"$c/repo/patches/eden-jit/MODE"
    cp "$FX/page_table.h" "$FX/page_table.cpp" "$c/eden/src/common/"
    cp "$FX/memory_excerpt.cpp" "$c/eden/src/core/memory.cpp"
    cp "$FX/emit_x64_memory.h" "$c/eden/$D/emit_x64_memory.h"
    cp "$FX/slow_lookup_excerpt.inc" "$c/eden/$D/emit_x64_memory.cpp.inc"
    cp "$FX/eden_page_lookup_excerpt.cpp" "$c/eden/$D/a64_emit_x64_memory.cpp"
    echo "// page reuse stand-in" >"$c/eden/part1.txt"
    (cd "$c/eden" && git init -q . && git add -A && "${G[@]}" commit -qm base)
    # part 1: a patch that appends a line to part1.txt
    (cd "$c/eden" && echo "// part 1 applied" >>part1.txt && "${G[@]}" commit -qam "part 1" \
        && git format-patch -1 --stdout >"$c/repo/patches/eden-jit/0001-part1.patch" && git reset -q --hard HEAD~1)
}
head_of() { (cd "$1" && git rev-parse HEAD); }

# 1. Both parts apply.
newcase a
bash scripts/apply-jit-pack.sh "$WORK/a/repo" "$WORK/a/eden" >"$WORK/a.out" 2>&1 || { cat "$WORK/a.out"; fail "apply failed"; }
grep -q "applied 0001-part1.patch" "$WORK/a.out" && grep -q "applied 0002-JIT-direct-page-table-copy.py" "$WORK/a.out" || { cat "$WORK/a.out"; fail "both parts were not applied"; }; ok
grep -q "part 1 applied" "$WORK/a/eden/part1.txt" && grep -q "ps4_direct_live" "$WORK/a/eden/src/common/page_table.h"; ok
(cd "$WORK/a/eden" && [ "$(git tag -l 'nxo-jit-pack-[0-9]*' | wc -l)" -eq 2 ] && [ "$(git rev-parse nxo-jit-pack)" = "$(git rev-parse nxo-jit-pack-2)" ] && [ "$(git rev-parse HEAD)" = "$(git rev-parse nxo-jit-pack-2)" ]) || fail "tags after apply"; ok
[ -z "$(cd "$WORK/a/eden" && git status --porcelain)" ] || fail "the tree is not clean after apply"; ok

# 1b. The report (0.9.4): both parts named as applied, in <repo root>/jit-pack-report.txt (or the file NXO_JIT_REPORT names).
[ -f "$WORK/a/repo/jit-pack-report.txt" ] || fail "no report written next to the repo root"
grep -q "^apply: 0001-part1.patch: applied" "$WORK/a/repo/jit-pack-report.txt" && grep -q "^apply: 0002-JIT-direct-page-table-copy.py: applied" "$WORK/a/repo/jit-pack-report.txt" || fail "report does not name both parts"; ok

# 2. Drop only the last part: part 1 stays, the direct table is gone, and the pack is still "in".
bash scripts/drop-jit-pack.sh "$WORK/a/eden" --last >/dev/null || fail "drop --last"
grep -q "part 1 applied" "$WORK/a/eden/part1.txt" || fail "part 1 went with --last"; ok
if grep -q "ps4_direct" "$WORK/a/eden/src/common/page_table.h"; then fail "part 2 stayed after --last"; fi; ok
(cd "$WORK/a/eden" && [ "$(git rev-parse nxo-jit-pack)" = "$(git rev-parse nxo-jit-pack-1)" ] && [ "$(git rev-parse HEAD)" = "$(git rev-parse nxo-jit-pack-1)" ]) || fail "tags after --last"; ok
# --last again: only one part left, nothing to do (exit 1), nothing changed.
before="$(head_of "$WORK/a/eden")"
if bash scripts/drop-jit-pack.sh "$WORK/a/eden" --last >/dev/null 2>&1; then fail "--last with one part left should exit 1"; fi
[ "$before" = "$(head_of "$WORK/a/eden")" ] || fail "--last with one part left changed the tree"; ok
# Whole drop.
bash scripts/drop-jit-pack.sh "$WORK/a/eden" >/dev/null || fail "drop all"
grep -q "part 1 applied" "$WORK/a/eden/part1.txt" && fail "part 1 stayed after the whole drop"
[ -z "$(cd "$WORK/a/eden" && git tag -l 'nxo-jit-pack*')" ] || fail "tags left after the whole drop"; ok
if bash scripts/drop-jit-pack.sh "$WORK/a/eden" >/dev/null 2>&1; then fail "drop with no pack should exit 1"; fi; ok

# 3. Whole drop straight after both parts.
newcase b
bash scripts/apply-jit-pack.sh "$WORK/b/repo" "$WORK/b/eden" >/dev/null 2>&1
bash scripts/drop-jit-pack.sh "$WORK/b/eden" >/dev/null || fail "drop all after two parts"
(cd "$WORK/b/eden" && git diff --quiet nxo-before-jit-pack && [ -z "$(git status --porcelain)" ]) || fail "tree differs from before the pack"; ok

# 4. Part 2 does not fit this Eden: part 1 stays, the build goes on.
newcase c
sed -i 's/data_raw.fetch_or(0b111)/data_raw.fetch_or(0b11)/' "$WORK/c/eden/src/common/page_table.h"
(cd "$WORK/c/eden" && "${G[@]}" commit -qam "different Eden")
bash scripts/apply-jit-pack.sh "$WORK/c/repo" "$WORK/c/eden" >"$WORK/c.out" 2>&1
grep -q "applied 0001-part1.patch" "$WORK/c.out" && grep -q "0002-JIT-direct-page-table-copy.py does not fit" "$WORK/c.out" || { cat "$WORK/c.out"; fail "part 2 refusal"; }; ok
grep -q "part 1 applied" "$WORK/c/eden/part1.txt" || fail "part 1 lost when part 2 did not fit"
if grep -q "ps4_direct" "$WORK/c/eden/src/common/page_table.h"; then fail "part 2 half applied"; fi
[ -z "$(cd "$WORK/c/eden" && git status --porcelain)" ] || fail "tree not clean after a refused part 2"
(cd "$WORK/c/eden" && [ "$(git rev-parse nxo-jit-pack)" = "$(git rev-parse nxo-jit-pack-1)" ]) || fail "tag after a refused part 2"; ok
grep -q "^apply: 0002-JIT-direct-page-table-copy.py: does not fit this Eden" "$WORK/c/repo/jit-pack-report.txt" && grep -q "^apply:   " "$WORK/c/repo/jit-pack-report.txt" && grep -q "^apply: 0001-part1.patch: applied" "$WORK/c/repo/jit-pack-report.txt" || { cat "$WORK/c/repo/jit-pack-report.txt"; fail "report of a refused part 2"; }; ok
# With only one part in, the CI's first step (--last) has nothing to do, the second one takes part 1 out.
if bash scripts/drop-jit-pack.sh "$WORK/c/eden" --last >/dev/null 2>&1; then fail "--last with part 1 only"; fi
bash scripts/drop-jit-pack.sh "$WORK/c/eden" >/dev/null; grep -q "part 1 applied" "$WORK/c/eden/part1.txt" && fail "part 1 stayed"; ok

# 5. Part 1 does not fit: nothing after it is tried.
newcase d
echo "// something else" >>"$WORK/d/eden/part1.txt"
(cd "$WORK/d/eden" && "${G[@]}" commit -qam "different Eden")
bash scripts/apply-jit-pack.sh "$WORK/d/repo" "$WORK/d/eden" >"$WORK/d.out" 2>&1
grep -q "0001-part1.patch does not fit" "$WORK/d.out" || { cat "$WORK/d.out"; fail "part 1 refusal"; }
grep -q "^apply: 0001-part1.patch: does not fit this Eden" "$WORK/d/repo/jit-pack-report.txt" && ! grep -q "0002" "$WORK/d/repo/jit-pack-report.txt" || fail "report of a refused part 1"; ok
if grep -q "applied 0002" "$WORK/d.out" || grep -q "ps4_direct" "$WORK/d/eden/src/common/page_table.h"; then fail "part 2 applied without part 1"; fi
(cd "$WORK/d/eden" && [ -z "$(git tag -l 'nxo-jit-pack-[0-9]*')" ] && ! git rev-parse -q --verify refs/tags/nxo-jit-pack >/dev/null) || fail "tags although nothing applied"; ok

# 6. MODE off: nothing happens.
newcase e
echo off >"$WORK/e/repo/patches/eden-jit/MODE"
bash scripts/apply-jit-pack.sh "$WORK/e/repo" "$WORK/e/eden" >"$WORK/e.out" 2>&1
grep -q "not applied" "$WORK/e.out" && [ -z "$(cd "$WORK/e/eden" && git status --porcelain)" ]; ok
grep -q "^apply: the pack is off" "$WORK/e/repo/jit-pack-report.txt" || fail "report with MODE off"; ok
echo "jit_pack_scripts_test: $pass checks passed"
