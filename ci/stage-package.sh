# ELF -> .pkg (and a copy of the ELF in elf/). See package-eden.sh for the layout and why.
set -euo pipefail
cd "$NXO_ROOT"
source tools/env-build.sh
BINDIR="$NXO_ROOT/sdk-dl/orbis-sdk-v1/sdk/bin/linux"

# PkgTool.Core (OpenOrbis) is a .NET program. Say what it is, so a failure here can be read.
set +e
{
    echo "--- PkgTool.Core"; ls -la "$BINDIR"
    command -v file >/dev/null && file "$BINDIR/PkgTool.Core" | cut -c1-240
    echo "--- dotnet runtimes on the runner"; dotnet --list-runtimes 2>&1 | sed -n '1,8p'
    echo "--- libssl / libicu"; ls /usr/lib/x86_64-linux-gnu/libssl* /usr/lib/x86_64-linux-gnu/libicu*.so* 2>&1 | sed -n '1,8p'
} 2>&1 | tee logs/pkgtool-env.txt
set -e

# No ICU needed (it only affects culture-specific text).
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

probe() { "$BINDIR/PkgTool.Core" sfo_new "$NXO_ROOT/dl/probe.sfo" >/dev/null 2>"$NXO_ROOT/logs/pkgtool-probe.txt"; }

if ! probe; then
    echo "PkgTool.Core does not start (see below); trying again with OpenSSL 1.1:"
    sed -n '1,6p' logs/pkgtool-probe.txt
    # An old .NET cannot use OpenSSL 3 ("No usable version of libssl was found"). Unpack Ubuntu 20.04's
    # libssl 1.1 into the work folder (nothing is installed on the system) and point .NET at it.
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
    export LD_LIBRARY_PATH="$NXO_ROOT/tools-dl/libssl1.1/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export CLR_OPENSSL_VERSION_OVERRIDE=1.1
    if ! probe; then
        echo "::error::PkgTool.Core still does not start, even with OpenSSL 1.1:"
        cat logs/pkgtool-probe.txt
        exit 1
    fi
    echo "PkgTool.Core runs with the private OpenSSL 1.1"
fi

bash package-eden.sh
ls -la out-eden elf
