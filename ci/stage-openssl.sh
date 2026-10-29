# OpenSSL for the PS4 -> prefix/ (docs/BUILDING.md, section 4).
set -euo pipefail
cd "$NXO_ROOT"
source tools/env-build.sh
V=3.6.2
mkdir -p deps
if [ ! -f "prefix/lib/libssl.a" ]; then
    [ -f "dl/openssl-$V.tar.gz" ] || curl -L --fail -o "dl/openssl-$V.tar.gz" \
        "https://github.com/openssl/openssl/releases/download/openssl-$V/openssl-$V.tar.gz"
    rm -rf "deps/openssl-$V"
    tar -xzf "dl/openssl-$V.tar.gz" -C deps
    cd "deps/openssl-$V"
    # The compiler flags go in CFLAGS (Configure misreads "-isysroot X" as arguments).
    CFLAGS="$PS4_CFLAGS" CC=clang AR=llvm-ar RANLIB=llvm-ranlib perl Configure linux-x86_64-clang \
        no-shared no-tests no-apps no-docs no-dso no-async no-afalgeng no-engine no-ui-console \
        no-secure-memory no-module --with-rand-seed=devrandom --prefix="$NXO_ROOT/prefix" --libdir=lib \
        --openssldir=/app0/ssl
    make -j"$JOBS" build_libs
    make install_dev
    cd "$NXO_ROOT"
    rm -rf "deps/openssl-$V"
fi
ls -la prefix/lib/libssl.a prefix/lib/libcrypto.a
