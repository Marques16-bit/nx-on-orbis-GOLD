# source this: tools for building C deps (OpenSSL, FFmpeg...) for the PS4.
# NXO_ROOT set (CI / Linux): every path comes from it, host tools from PATH (plus NXO_LLVM_BIN).
# NXO_ROOT unset: the original author's Git Bash layout on Windows, unchanged.
if [ -n "${NXO_ROOT:-}" ]; then
    R="$NXO_ROOT"
    if [ -n "${NXO_LLVM_BIN:-}" ]; then export PATH="$NXO_LLVM_BIN:$PATH"; fi
else
    R=C:/Users/alejo/eden-ps4
    P=/c/Users/alejo/eden-ps4/tools/msys-perl/usr
    export PERL5LIB=$P/share/perl5/core_perl:$P/lib/perl5/core_perl
    export PATH="/c/Users/alejo/eden-ps4/tools/bin:$P/bin:/c/Users/alejo/eden-ps4/tools/usr/bin:/c/Users/alejo/soh-ps4/tools/llvm/bin:/c/Users/alejo/soh-ps4/tools/cmake-4.4.3-windows-x86_64/bin:/c/Users/alejo/soh-ps4/tools/ninja:/c/Users/alejo/soh-ps4/tools/glslang/bin:$PATH"
fi
export PS4_SDK=$R/sdk-dl/orbis-sdk-v1/sdk
export PS4_COMPAT=$R/sdk-dl/orbis-sdk-v1/orbis-compat
export PS4_OVERLAY=$R/toolchain/include-overlay
export PS4_PREFIX=$R/prefix
export PS4_CFLAGS="--target=x86_64-pc-freebsd12-elf -O2 -fPIC -funwind-tables -D__PS4__ -D__ORBIS__ -D_BSD_SOURCE=1 -U__FreeBSD__ -isysroot $PS4_SDK -isystem $PS4_OVERLAY -isystem $PS4_COMPAT/include -isystem $PS4_SDK/include -include orbis_prefix.h"
# Linux host: clang's FreeBSD driver would also search the host's /usr/include (see
# toolchain/runtimes-ps4.cmake); on Windows there is none and the flags are unchanged.
if [ -n "${NXO_ROOT:-}" ]; then export PS4_CFLAGS="$PS4_CFLAGS -nostdlibinc"; fi
unset R
