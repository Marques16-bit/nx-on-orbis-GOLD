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
cmake --build build-eden --target eden-ps4 -j"$JOBS"
ls -la build-eden/bin/eden-ps4
cat build-eden/eden-ps4-build-id.txt 2>/dev/null || true
if command -v ccache >/dev/null 2>&1; then ccache -s | head -n 12 || true; fi
