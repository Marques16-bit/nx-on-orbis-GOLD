# Eden + this port's patches + the frontend -> build-eden/bin/eden-ps4 (docs/BUILDING.md, section 5).
set -euo pipefail
cd "$NXO_ROOT"
source tools/env-build.sh
unset OO_PS4_TOOLCHAIN

[ -d deps/eden ] || bash scripts/fetch-eden.sh

extra=()
if command -v ccache >/dev/null 2>&1; then
    extra+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
# A release build (tag vX.Y.Z) carries its version in the build id (first line of boot.log).
if [ -n "${NXO_VERSION:-}" ]; then
    extra+=(-DEDEN_PS4_VERSION="$NXO_VERSION")
fi
bash configure-eden.sh "${extra[@]}"

# The cross build must never see the runner's own (Linux) headers or libraries. Check now, in
# minutes, instead of finding out an hour into the compile.
leaks="$(grep -n -E -e '-isystem /usr/(local/)?include|-I ?/usr/(local/)?include|/usr/lib/x86_64-linux-gnu|/usr/lib64' \
         build-eden/build.ninja | sed -n '1,8p' | cut -c1-240 || true)"
if [ -n "$leaks" ]; then
    echo "::error::host (Linux) include/library paths leaked into the PS4 build:"
    echo "$leaks"
    echo "--- CMake cache entries that point at the host:"
    grep -E '^[A-Za-z0-9_]+:(PATH|FILEPATH|STRING)=/usr/' build-eden/CMakeCache.txt | sed -n '1,40p' || true
    exit 1
fi
echo "configure ok: no host include/library paths in the build"
# -k 0: keep going after a failure; with the compiler cache every object that did build is kept, so a
# run that fails still pays for the next one, and one run lists all the errors.
set +e
cmake --build build-eden --target eden-ps4 -j"$JOBS" -- -k 0 2>&1 | tee build-eden-last.log
status=${PIPESTATUS[0]}
set -e
# The JIT pack (patches/eden-jit/) is optional. When the build failed inside Dynarmic, or at the link on one of the pack's symbols, or in
# the page table code the direct table (part 2) edits, and the pack is in the tree, it is taken out in two steps, each followed by another
# build: first only the last part (0.9.3: the direct table; part 1, page reuse, stays), then the whole pack. A pack that does not
# compile costs the pack, not the release. The errors of each step are kept in jit-pack-errors.log (the "jit-pack-errors" artifact).
pack_error='src/dynarmic/[^ :]*:[0-9]+:[0-9]+: (fatal )?error|A64EdenPageLookup|ps4_page_reuse|ps4_direct|PS4DirectOk|ZeroDirect|SyncDirect|DirectSlot|common/page_table\.(h|cpp):[0-9]+:[0-9]+: (fatal )?error'
for stage in last all; do
    if [ "$status" = 0 ] || ! grep -qE "$pack_error" build-eden-last.log; then
        break
    fi
    if [ "$stage" = last ]; then
        bash scripts/drop-jit-pack.sh deps/eden --last || continue
    else
        bash scripts/drop-jit-pack.sh deps/eden || break
    fi
    { echo "The build failed with the JIT pack ($stage) in; these are the errors:"; grep -n -E -B2 -A8 'error' build-eden-last.log | head -n 400; } >> jit-pack-errors.log || true
    echo "::warning::the JIT pack did not compile; building again without its $stage part (the errors are in jit-pack-errors.log)"
    sed -n '1,40p' jit-pack-errors.log || true
    set +e
    cmake --build build-eden --target eden-ps4 -j"$JOBS" -- -k 0 2>&1 | tee build-eden-last.log
    status=${PIPESTATUS[0]}
    set -e
done
if [ "$status" != 0 ] && grep -q "sceKernelGet.*Temperature" build-eden-last.log; then
    echo "::warning::the SDK has no temperature functions: building the HUD without temperatures"
    cmake -S deps/eden -B build-eden -DEDEN_PS4_HUD_TEMPERATURE=OFF >/dev/null
    cmake --build build-eden --target eden-ps4 -j"$JOBS"
elif [ "$status" != 0 ]; then
    exit "$status"
fi
ls -la build-eden/bin/eden-ps4
cat build-eden/eden-ps4-build-id.txt 2>/dev/null || true
if command -v ccache >/dev/null 2>&1; then ccache -s | head -n 12 || true; fi
