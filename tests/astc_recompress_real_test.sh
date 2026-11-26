#!/usr/bin/env bash
# Host test of patches/eden-src/astc_recompress.h against Eden's OWN ASTC decoder and BC1/BC3 compressors (0.9.8). The slab conversion of
# patch 0036 has to give the bytes of the old whole-level conversion; this builds Eden's astc.cpp, bcn.cpp and stb_dxt.cpp on a PC and
# compares the two ways on random images (tests/astc_recompress_real_test.cpp). Not part of CI (CI has no Eden tree at the host-test
# step; tests/astc_recompress_checks.h is the CI-hosted half, with stand-in codecs). Run from the repository root:
#   bash tests/astc_recompress_real_test.sh [EDEN_TREE]
# EDEN_TREE defaults to deps/eden (scripts/fetch-eden.sh). Eden's code needs Boost.Container headers and fmt's: if they are not in the
# system include path, give BOOST_ROOT (a Boost source tree with libs/*/include) and FMT_INC (fmt's include directory).
set -euo pipefail
cd "$(dirname "$0")/.."
EDEN="${1:-deps/eden}"
[ -f "$EDEN/src/video_core/textures/astc.cpp" ] || { echo "no Eden tree at $EDEN (run scripts/fetch-eden.sh, or give the path)"; exit 2; }
[ -f "$EDEN/externals/stb/stb_dxt.cpp" ] || { echo "no stb_dxt.cpp in $EDEN/externals/stb"; exit 2; }
INC=()
if [ -n "${BOOST_ROOT:-}" ]; then
    for d in "$BOOST_ROOT"/libs/*/include; do INC+=("-I$d"); done
fi
[ -n "${FMT_INC:-}" ] && INC+=("-I$FMT_INC")
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
g++ -std=c++20 -O2 -DNDEBUG -w -Itests/astc_recompress_shim -I"$EDEN/src" -I"$EDEN/externals/stb" "${INC[@]}" \
    tests/astc_recompress_real_test.cpp "$EDEN/src/video_core/textures/astc.cpp" "$EDEN/src/video_core/textures/bcn.cpp" \
    "$EDEN/externals/stb/stb_dxt.cpp" -o "$OUT/astc_real"
"$OUT/astc_real"
