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
cmake --build build-eden --target eden-ps4 -j"$JOBS" -- -k 0
ls -la build-eden/bin/eden-ps4
cat build-eden/eden-ps4-build-id.txt 2>/dev/null || true
if command -v ccache >/dev/null 2>&1; then ccache -s | head -n 12 || true; fi
