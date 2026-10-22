# Downloads the PS4 SDK (orbis-porting-kit "orbis-sdk-v1") and the OpenOrbis toolchain (only for its
# Linux tools and the samples/piglet modules the package uses), and reports their layout.
# Needs the gh CLI with GH_TOKEN set (always the case on GitHub-hosted runners).
set -euo pipefail
cd "$NXO_ROOT"
mkdir -p sdk-dl tools-dl dl

# fetch_release REPO OUTDIR TAG...: downloads all the assets of the first tag that exists (or, for a
# release without assets, the tag's source archive).
fetch_release() {
    local repo="$1" out="$2" tag; shift 2
    rm -rf "$out"; mkdir -p "$out"
    for tag in "$@"; do
        if gh release download "$tag" --repo "$repo" --dir "$out" --clobber 2>/dev/null \
           && [ -n "$(ls -A "$out")" ]; then
            echo "$repo $tag: $(ls "$out" | tr '\n' ' ')"; return 0
        fi
        if gh release download "$tag" --repo "$repo" --dir "$out" --archive=tar.gz --clobber 2>/dev/null \
           && [ -n "$(ls -A "$out")" ]; then
            echo "$repo $tag (source archive): $(ls "$out" | tr '\n' ' ')"; return 0
        fi
    done
    echo "::error::no release found in $repo (tried: $*)"; return 1
}

# extract TO FILE...: unpacks every archive among the given files into TO
extract() {
    local to="$1" a; shift; mkdir -p "$to"
    for a in "$@"; do
        case "$a" in
            *.zip) unzip -q -o "$a" -d "$to" ;;
            *.tar.gz|*.tgz) tar -xzf "$a" -C "$to" ;;
            *.tar.xz) tar -xJf "$a" -C "$to" ;;
            *.tar.bz2) tar -xjf "$a" -C "$to" ;;
            *) echo "(not an archive, skipped: $a)" ;;
        esac
    done
}

# ---- 1. orbis-sdk-v1  ->  sdk-dl/orbis-sdk-v1/{sdk,orbis-compat,toolchain,...}
if [ ! -f sdk-dl/orbis-sdk-v1/toolchain/orbis-sdk.cmake ]; then
    fetch_release orbis-ports/orbis-porting-kit dl/sdk orbis-sdk-v1
    rm -rf sdk-dl/_x sdk-dl/orbis-sdk-v1
    extract sdk-dl/_x dl/sdk/*
    cmake_file="$(find sdk-dl/_x -path '*/toolchain/orbis-sdk.cmake' | head -n 1)"
    if [ -z "$cmake_file" ]; then
        echo "::error::toolchain/orbis-sdk.cmake is not in the downloaded SDK; what is there:"
        find sdk-dl/_x -maxdepth 4 | sort | head -n 80
        exit 1
    fi
    mv "$(dirname "$(dirname "$cmake_file")")" sdk-dl/orbis-sdk-v1
    rm -rf sdk-dl/_x
fi
SDKROOT="$NXO_ROOT/sdk-dl/orbis-sdk-v1"

# ---- 2. OpenOrbis (its Linux archive when the release has one per platform)
if [ ! -d tools-dl/OpenOrbis/samples/piglet ]; then
    fetch_release OpenOrbis/OpenOrbis-PS4-Toolchain dl/oo v0.5.4 0.5.4
    rm -rf tools-dl/_x tools-dl/OpenOrbis
    mapfile -t linux_assets < <(ls dl/oo/* | grep -i linux || true)
    [ "${#linux_assets[@]}" -gt 0 ] || mapfile -t linux_assets < <(ls dl/oo/*)
    extract tools-dl/_x "${linux_assets[@]}"
    piglet="$(find tools-dl/_x -type d -path '*/samples/piglet' | head -n 1)"
    if [ -z "$piglet" ]; then
        echo "::error::samples/piglet is not in the OpenOrbis download; what is there:"
        find tools-dl/_x -maxdepth 4 | sort | head -n 80
        exit 1
    fi
    mv "$(dirname "$(dirname "$piglet")")" tools-dl/OpenOrbis
    rm -rf tools-dl/_x
fi

# ---- 3. the packaging tools into the SDK's own bin/linux (where the SDK's CMake looks for them)
mkdir -p "$SDKROOT/sdk/bin/linux"
oo_bin="$(find tools-dl/OpenOrbis -type d -path '*/bin/linux' | head -n 1)"
if [ -n "$oo_bin" ]; then
    cp -n "$oo_bin"/* "$SDKROOT/sdk/bin/linux/" 2>/dev/null || true
fi
chmod +x "$SDKROOT"/sdk/bin/linux/* 2>/dev/null || true

# ---- 4. orbis-compat's library (normally shipped built; try to build it if it is not)
compat="$SDKROOT/orbis-compat"
if [ ! -f "$compat/build/liborbis-compat.a" ] && [ -f "$compat/CMakeLists.txt" ]; then
    echo "liborbis-compat.a not shipped: trying to build it"
    cmake -S "$compat" -B "$compat/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$SDKROOT/toolchain/orbis-sdk.cmake" || true
    cmake --build "$compat/build" -j"$JOBS" || true
fi

# ---- 5. the layout report (kept in logs/ and in the job summary): the first thing to read if a
# later stage cannot find something. Informational only: a grep that finds nothing must not stop it.
set +e
{
    echo "SDK root: $SDKROOT"
    echo "--- top levels"; (cd "$SDKROOT" && find . -maxdepth 2 | sort | head -n 80)
    echo "--- sdk/bin/linux"; ls -la "$SDKROOT/sdk/bin/linux" 2>&1 | head -n 30
    echo "--- liborbis-compat.a / crt1.o / piglet modules"
    ls -la "$compat/build/liborbis-compat.a" "$SDKROOT/sdk/lib/crt1.o" \
        tools-dl/OpenOrbis/samples/piglet/sce_module 2>&1 | head -n 30
    for f in toolchain/orbis-sdk.cmake orbis-compat/cmake/ps4-openorbis.cmake; do
        echo "--- $f (host handling)"
        grep -n -i "HOST\|APPLE\|UNIX\|WIN32\|BINDIR" "$SDKROOT/$f" 2>&1 | head -n 20
    done
} 2>&1 | tee logs/sdk-layout.txt >/dev/null
set -e

missing=0
for f in "$SDKROOT/toolchain/orbis-sdk.cmake" "$SDKROOT/sdk/lib/crt1.o" "$compat/build/liborbis-compat.a" \
         "$NXO_ROOT/tools-dl/OpenOrbis/samples/piglet/sce_module/libc.prx"; do
    [ -e "$f" ] || { echo "::error::missing after the SDK stage: $f"; missing=1; }
done
for t in create-fself create-gp4; do
    [ -x "$SDKROOT/sdk/bin/linux/$t" ] || { echo "::error::missing Linux tool: sdk/bin/linux/$t"; missing=1; }
done
if [ "$missing" = 1 ]; then cat logs/sdk-layout.txt; exit 1; fi
echo "SDK stage ok (layout in logs/sdk-layout.txt)"
