#!/usr/bin/env bash
# The JIT pack (patches/eden-jit/) is optional. When the build failed (argument 1: its exit status) and the pack is in the Eden checkout, it is taken
# out one part at a time, the last part first, each followed by another build: the direct table (part 2) goes before page reuse (part 1) does, and a pack
# that does not compile costs the pack, not the release. 0.9.4: this no longer waits for an error that looks like the pack's (the 0.9.3 build left part 2
# out and nothing said why): whatever the error, the pack is the first suspect, since the build without it was known to work. Each step's errors are kept
# in jit-pack-errors.log (the "jit-pack-errors" artifact) and, shortened, in jit-pack-report.txt, which package-eden.sh puts in the package and boot.log
# quotes: the console's own log then says which part is in and why one is not.
# Exit status: that of the last build (0 = it worked). Run from ci/stage-eden.sh with NXO_ROOT and JOBS set; tests/pack_fallback_test.sh runs it with a fake cmake.
set -uo pipefail
status="${1:-0}"
cd "$NXO_ROOT"
EDEN="${NXO_EDEN_DIR:-deps/eden}"
report="${NXO_JIT_REPORT:-$NXO_ROOT/jit-pack-report.txt}"
log="${NXO_BUILD_LOG:-build-eden-last.log}"
touch "$report"
pack_parts() { git -C "$EDEN" tag -l 'nxo-jit-pack-[0-9]*' 2>/dev/null | wc -l | tr -d ' '; }
steps=0
while [ "$status" != 0 ] && [ "$(pack_parts)" -gt 0 ] && [ "$steps" -lt 4 ]; do
    steps=$((steps + 1))
    parts_before="$(pack_parts)"
    if [ "$parts_before" -gt 1 ]; then stage=last; what="the last part"; else stage=all; what="the whole pack"; fi
    short="$(grep -E 'error|undefined|multiple definition' "$log" | grep -v '^ninja' | sed 's#.*/deps/eden/##' | cut -c1-220 | head -n 8 || true)"
    if [ "$stage" = last ]; then
        bash scripts/drop-jit-pack.sh "$EDEN" --last || break
    else
        bash scripts/drop-jit-pack.sh "$EDEN" || break
    fi
    { echo "The build failed with the JIT pack ($parts_before part(s)) in; $what is taken out. These are the errors:"; grep -n -E -B2 -A8 'error' "$log" | head -n 400; } >> jit-pack-errors.log || true
    { echo "build: it failed with $parts_before pack part(s) in; took out $what. First errors:"; printf '%s\n' "$short" | sed 's/^/build:   /'; } >> "$report"
    echo "::warning::the build failed with the JIT pack in; building again without $what (the errors are in jit-pack-errors.log)"
    sed -n '1,40p' jit-pack-errors.log || true
    cmake --build build-eden --target eden-ps4 -j"${JOBS:-2}" -- -k 0 2>&1 | tee "$log"
    status=${PIPESTATUS[0]}
done
echo "final: pack parts in the build: $(pack_parts)$([ "$status" = 0 ] || echo ' (the build still failed)')" >> "$report"
echo "--- jit-pack-report.txt:"
cat "$report"
exit "$status"
