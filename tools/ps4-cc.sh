#!/usr/bin/env bash
# clang for the PS4 with the eden-ps4 C flags, for autoconf-style configures (FFmpeg).
# Link probes really link against the SDK's libc/libkernel stubs (never run), entry = main.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/env-build.sh"
LIBCXX18="${NXO_ROOT:-C:/Users/alejo/eden-ps4}/libcxx18"
args=()
link=true
for a in "$@"; do
    case "$a" in
        -c|-E|-S|-M|-MM|--version) link=false; args+=("$a") ;;
        -lm|-lpthread|-pthread) ;;
        *) args+=("$a") ;;
    esac
done
if $link; then
    # The same link line as toolchain/eden-ps4.cmake (orbis-tls.ld, crt1.o), so probes see what a real link sees.
    args+=(-nostdlib -fuse-ld=lld -pie -Wl,-m,elf_x86_64 -Wl,--script="$PS4_COMPAT/cmake/orbis-tls.ld"
           -Wl,--eh-frame-hdr -Wl,--no-rosegment -L "$LIBCXX18/lib" -L "$PS4_SDK/lib"
           -Wl,--whole-archive "$PS4_COMPAT/build/liborbis-compat.a" -Wl,--no-whole-archive
           -lc++ -lc++abi -lunwind -lc -lkernel "$PS4_SDK/lib/crt1.o")
fi
exec clang $PS4_CFLAGS -ffunction-sections -fdata-sections "${args[@]}"
