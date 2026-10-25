#!/usr/bin/env bash
# build-eden/eden-ps4 (ELF) -> out-eden/IV0000-EDPS00001_00-EDENPS4000000000.pkg
#
# Signed and laid out like OpenOrbis's own samples (paid 0x3800000000000011, default authinfo,
# sce_module/libc.prx + libSceFios2.prx, sce_sys/about/right.sprx, SFO category gd): the only
# layout tested on Alejo's PS4 Pro (FW 12.02, GoldHEN) that both loads and gets the full
# ~4.4 GiB of direct memory (eden-probe rounds 1-3, NOTAS.md).
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
SDK="$ROOT/sdk-dl/orbis-sdk-v1/sdk"
# Windows (Git Bash, the original machine) or Linux (CI). OO_TOOLCHAIN: the OpenOrbis folder that
# holds samples/piglet.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) HOST=windows; EXE=.exe; WIN=1 ;;
    *) HOST=linux; EXE=""; WIN=0 ;;
esac
BIN="$SDK/bin/$HOST"
OO="${OO_TOOLCHAIN:-C:/Users/alejo/soh-ps4/tools/OpenOrbis/OpenOrbis/PS4Toolchain}"
PY="$(command -v python3 || command -v python)"
winpath() { if [ "$WIN" = 1 ]; then cygpath -m "$1"; else printf '%s' "$1"; fi; }
# PkgTool.Core is a native program in some OpenOrbis builds and a .NET assembly in others.
pkgtool() {
    if [ -x "$BIN/PkgTool.Core$EXE" ]; then "$BIN/PkgTool.Core$EXE" "$@"
    elif [ -f "$BIN/PkgTool.Core.dll" ]; then dotnet "$BIN/PkgTool.Core.dll" "$@"
    else echo "PkgTool.Core not found in $BIN" >&2; return 127; fi
}
export DOTNET_ROLL_FORWARD=LatestMajor
export OO_PS4_TOOLCHAIN="$(winpath "$SDK")"

TITLE="NX on Orbis"
TITLE_ID="EDPS00001"
CID="IV0000-${TITLE_ID}_00-EDENPS4000000000"
ELF="${ELF:-$ROOT/build-eden/bin/eden-ps4}"
ST="$ROOT/stage-eden"
OUT="$ROOT/out-eden"

[ -f "$ELF" ] || { echo "missing $ELF"; exit 1; }
rm -rf "$ST"; mkdir -p "$ST/sce_sys/about" "$ST/sce_module" "$OUT"
# The package's icon (512x512) and the background of its page on the home screen (1920x1080),
# drawn by tools/make-icons.py. Without them the old probe icon is made.
if [ -f "$ROOT/assets/icon0.png" ]; then
    cp "$ROOT/assets/icon0.png" "$ST/sce_sys/icon0.png"
else
    "$PY" "$ROOT/probe/make-icon.py" "$ST/sce_sys/icon0.png" "NX"
fi
EXTRA_FILES=""
if [ -f "$ROOT/assets/pic1.png" ]; then
    cp "$ROOT/assets/pic1.png" "$ST/sce_sys/pic1.png"
    EXTRA_FILES=" sce_sys/pic1.png"
fi
"$BIN/create-fself$EXE" -in="$(winpath "$ELF")" -out="$(winpath "$ST/x.oelf")" \
    --eboot "$(winpath "$ST/eboot.bin")" --paid 0x3800000000000011 >/dev/null
rm -f "$ST/x.oelf"
cp "$OO/samples/piglet/sce_sys/about/right.sprx" "$ST/sce_sys/about/"
cp "$OO/samples/piglet/sce_module/libc.prx" "$OO/samples/piglet/sce_module/libSceFios2.prx" "$ST/sce_module/"
FILES="eboot.bin sce_sys/param.sfo sce_sys/icon0.png$EXTRA_FILES sce_sys/about/right.sprx sce_module/libc.prx sce_module/libSceFios2.prx"
# The Homebrew Menu (switchbrew/nx-hbmenu v3.6.1, ISC licence) as the test program when the user
# has no game in roms/: it needs no keys or firmware. create-gp4 only takes a few root folders.
mkdir -p "$ST/assets/misc"
cp "$ROOT/testroms/hbmenu.nro" "$ST/assets/misc/hbmenu.nro"
cp "$ROOT/testroms/hbmenu-LICENSE.txt" "$ST/assets/misc/hbmenu-LICENSE.txt"
FILES="$FILES assets/misc/hbmenu.nro assets/misc/hbmenu-LICENSE.txt"
(
    cd "$ST"
    SFO=sce_sys/param.sfo
    pkgtool sfo_new $SFO >/dev/null
    s() { pkgtool sfo_setentry $SFO "$1" --type "$2" --maxsize "$3" --value "$4" >/dev/null; }
    s APP_TYPE Integer 4 1
    s APP_VER Utf8 8 "01.00"
    s ATTRIBUTE Integer 4 0
    s CATEGORY Utf8 4 gd
    s SYSTEM_VER Integer 4 0
    s CONTENT_ID Utf8 48 "$CID"
    s DOWNLOAD_DATA_SIZE Integer 4 0
    s TITLE Utf8 128 "$TITLE"
    s TITLE_ID Utf8 12 "$TITLE_ID"
    s VERSION Utf8 8 "01.00"
    "$BIN/create-gp4$EXE" -out pkg.gp4 --content-id="$CID" --files "$FILES" >/dev/null
    pkgtool pkg_build pkg.gp4 "$(winpath "$OUT")" >/dev/null
)
mkdir -p "$ROOT/elf"
cp "$ELF" "$ROOT/elf/eden-ps4-$(date +%Y%m%d-%H%M).elf"
ls -la "$OUT"
