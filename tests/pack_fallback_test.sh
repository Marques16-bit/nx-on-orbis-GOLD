#!/usr/bin/env bash
# Host test of ci/pack-fallback.sh (0.9.4) with a fake cmake: after a failed build the JIT pack comes out one part at a time, last part first, whatever the
# error looks like, and the report says what happened. Run from the repository root.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
pass=0
ok() { pass=$((pass + 1)); }
fail() { echo "FAIL: $*"; exit 1; }
G=(git -c user.name=t -c user.email=t@t)

# A fake Eden checkout with two pack parts (tags as scripts/apply-jit-pack.sh leaves them): part 1 appends to part1.txt, part 2 to part2.txt.
newcase() { # $1 = name
    local c="$WORK/$1"
    mkdir -p "$c/bin" "$c/root/deps/eden" "$c/root/scripts" "$c/root/ci" "$c/root/build-eden"
    cp "$ROOT/scripts/drop-jit-pack.sh" "$c/root/scripts/"
    cp "$ROOT/ci/pack-fallback.sh" "$c/root/ci/"
    (cd "$c/root/deps/eden" && git init -q . && echo base >base.txt && git add -A && "${G[@]}" commit -qm base && git tag nxo-before-jit-pack \
        && echo p1 >part1.txt && git add -A && "${G[@]}" commit -qm p1 && git tag nxo-jit-pack-1 \
        && echo p2 >part2.txt && git add -A && "${G[@]}" commit -qm p2 && git tag nxo-jit-pack-2 && git tag nxo-jit-pack nxo-jit-pack-2)
    # The fake cmake: fails with FAKE_FAIL=part2 while part2.txt exists, FAKE_FAIL=part1 while part1.txt exists, FAKE_FAIL=always always.
    cat >"$c/bin/cmake" <<'CM'
#!/usr/bin/env bash
case "${FAKE_FAIL:-}" in
always) bad=1 ;;
part2) [ -e "$NXO_ROOT/deps/eden/part2.txt" ] && bad=1 || bad=0 ;;
part1) [ -e "$NXO_ROOT/deps/eden/part1.txt" ] && bad=1 || bad=0 ;;
*) bad=0 ;;
esac
echo "building..."
if [ "${bad:-0}" = 1 ]; then
    echo "/runner/work/deps/eden/src/core/memory.cpp:123:9: error: use of undeclared identifier 'boom'"
    exit 1
fi
exit 0
CM
    chmod +x "$c/bin/cmake"
}
run() { # $1 = case, $2 = FAKE_FAIL; leaves the exit status in $rc
    local c="$WORK/$1"
    set +e
    (cd "$c/root" && PATH="$c/bin:$PATH" NXO_ROOT="$c/root" JOBS=2 FAKE_FAIL="$2" NXO_BUILD_LOG="$c/root/build.log" \
        bash -c 'echo "/runner/work/deps/eden/src/core/memory.cpp:123:9: error: use of undeclared identifier boom" > build.log; bash ci/pack-fallback.sh 1' >"$c/out.txt" 2>&1)
    rc=$?
    set -e
}
tags() { git -C "$WORK/$1/root/deps/eden" tag -l 'nxo-jit-pack-[0-9]*' | wc -l | tr -d ' '; }

# 1. Part 2 is the problem: only part 2 comes out; part 1 stays; the second build works; the error does not have to look like the pack's.
newcase a; run a part2
[ "$rc" = 0 ] || { cat "$WORK/a/out.txt"; fail "part2 failure: exit $rc"; }; ok
[ "$(tags a)" = 1 ] && [ -e "$WORK/a/root/deps/eden/part1.txt" ] && [ ! -e "$WORK/a/root/deps/eden/part2.txt" ] || fail "part2 failure: wrong parts left"; ok
R="$WORK/a/root/jit-pack-report.txt"
grep -q "^build: it failed with 2 pack part(s) in; took out the last part" "$R" && grep -q "^build:   src/core/memory.cpp:123:9: error" "$R" && grep -q "^final: pack parts in the build: 1$" "$R" || { cat "$R"; fail "part2 failure: report"; }; ok
grep -q "memory.cpp:123:9: error" "$WORK/a/root/jit-pack-errors.log" || fail "errors not kept in jit-pack-errors.log"; ok

# 2. Part 1 is the problem: part 2 comes out first (still failing), then the whole pack.
newcase b; run b part1
[ "$rc" = 0 ] || { cat "$WORK/b/out.txt"; fail "part1 failure: exit $rc"; }; ok
[ "$(tags b)" = 0 ] && [ ! -e "$WORK/b/root/deps/eden/part1.txt" ] || fail "part1 failure: pack not gone"; ok
grep -q "took out the last part" "$WORK/b/root/jit-pack-report.txt" && grep -q "took out the whole pack" "$WORK/b/root/jit-pack-report.txt" && grep -q "^final: pack parts in the build: 0$" "$WORK/b/root/jit-pack-report.txt" || { cat "$WORK/b/root/jit-pack-report.txt"; fail "part1 failure: report"; }; ok

# 3. The failure is not the pack's: everything comes out and the build still fails; the exit status says so and the report says so.
newcase c; run c always
[ "$rc" != 0 ] || fail "an unrelated failure was reported as success"; ok
grep -q "^final: pack parts in the build: 0 (the build still failed)$" "$WORK/c/root/jit-pack-report.txt" || { cat "$WORK/c/root/jit-pack-report.txt"; fail "always failure: report"; }; ok

# 4. No pack in the tree: nothing to take out, the status passes through.
newcase d
(cd "$WORK/d/root/deps/eden" && git tag -d nxo-jit-pack nxo-jit-pack-1 nxo-jit-pack-2 >/dev/null)
run d always
[ "$rc" != 0 ] && grep -q "^final: pack parts in the build: 0 (the build still failed)$" "$WORK/d/root/jit-pack-report.txt" && ! grep -q "^build:" "$WORK/d/root/jit-pack-report.txt" || fail "no pack: should only pass the status on"; ok

# 5. The first build worked: nothing happens.
newcase e
set +e
(cd "$WORK/e/root" && PATH="$WORK/e/bin:$PATH" NXO_ROOT="$WORK/e/root" bash ci/pack-fallback.sh 0 >"$WORK/e/out.txt" 2>&1); rc=$?
set -e
[ "$rc" = 0 ] && [ "$(tags e)" = 2 ] && grep -q "^final: pack parts in the build: 2$" "$WORK/e/root/jit-pack-report.txt"; ok
echo "pack_fallback_test: $pass checks passed"
