# Sourced by every CI stage: everything the scripts need to know about the machine.
export NXO_ROOT="${NXO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
export NXO_LLVM_BIN="${NXO_LLVM_BIN:-/usr/lib/llvm-18/bin}"
export JOBS="${JOBS:-$(nproc)}"
# The OpenOrbis release (samples/piglet and the Linux tools), unpacked by stage-sdk.sh.
export OO_TOOLCHAIN="$NXO_ROOT/tools-dl/OpenOrbis"
export PATH="$NXO_LLVM_BIN:$PATH"
mkdir -p "$NXO_ROOT/logs" "$NXO_ROOT/stamps" "$NXO_ROOT/dl" "$NXO_ROOT/.ccache"

# summary "text": a line in the job summary on GitHub (and nowhere when run by hand)
summary() { printf '%s\n' "$*" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"; }
