# ELF -> .pkg (and a copy of the ELF in elf/). See package-eden.sh for the layout and why.
set -euo pipefail
cd "$NXO_ROOT"
source tools/env-build.sh
BINDIR="$NXO_ROOT/sdk-dl/orbis-sdk-v1/sdk/bin/linux"

# PkgTool.Core (OpenOrbis) is a self-contained .NET program. Say what it is, so a failure here can be read.
set +e
{
    echo "--- PkgTool.Core"; ls -la "$BINDIR"
    command -v file >/dev/null && file "$BINDIR/PkgTool.Core" | cut -c1-240
    echo "--- libssl on the runner"; ls /usr/lib/x86_64-linux-gnu/ 2>/dev/null | grep -E '^libssl|^libcrypto' | sed -n '1,8p'
} 2>&1 | tee logs/pkgtool-env.txt
set -e

# No ICU needed (it only affects culture-specific text).
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

# PkgTool.Core carries a .NET old enough to look only for OpenSSL 1.x. It starts and builds the SFO
# without cryptography, then dies at pkg_build with "No usable version of libssl was found" because
# Ubuntu 24.04 only has OpenSSL 3. So it is always given OpenSSL 1.1: Ubuntu 20.04's package, unpacked
# into the work folder (nothing is installed on the system), unless the system already has one.
have11="$(ldconfig -p 2>/dev/null | grep -c 'libssl\.so\.1\.1' || true)"
if [ "${have11:-0}" = 0 ]; then
    mkdir -p tools-dl/libssl1.1
    got=0
    for url in \
        http://archive.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2_amd64.deb \
        http://security.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb \
        http://security.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2.23_amd64.deb; do
        if curl -fsSL -o dl/libssl1.1.deb "$url"; then got=1; echo "downloaded $url"; break; fi
    done
    if [ "$got" = 0 ]; then echo "::error::could not download libssl 1.1 (all URLs failed)"; exit 1; fi
    dpkg-deb -x dl/libssl1.1.deb tools-dl/libssl1.1
    ssl_dir="$NXO_ROOT/tools-dl/libssl1.1/usr/lib/x86_64-linux-gnu"
    if [ ! -f "$ssl_dir/libssl.so.1.1" ] || [ ! -f "$ssl_dir/libcrypto.so.1.1" ]; then
        echo "::error::the libssl 1.1 package does not hold libssl.so.1.1 and libcrypto.so.1.1:"; ls -la "$ssl_dir"; exit 1
    fi
    export LD_LIBRARY_PATH="$ssl_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
export CLR_OPENSSL_VERSION_OVERRIDE=1.1
echo "PkgTool.Core will use OpenSSL 1.1 (LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-})"

bash package-eden.sh
ls -la out-eden elf
