#!/usr/bin/env bash
# The PS4 build, stage by stage.   Usage: bash ci/run.sh [stage ...]      (default: all stages)
#
#   sdk       orbis-sdk-v1 + OpenOrbis tools        -> sdk-dl/, tools-dl/
#   libcxx    LLVM 18 libc++ / libc++abi            -> libcxx18/
#   openssl   OpenSSL                               -> prefix/
#   ffmpeg    FFmpeg (h264, vp8, vp9)               -> prefix/
#   eden      Eden + patches + frontend             -> build-eden/bin/eden-ps4
#   package   .pkg and ELF copy                     -> out-eden/, elf/
#
# The first four are kept in the workflow cache: a stage that finished leaves stamps/<stage> and is
# skipped next time. A failing stage prints the end of its log and stops the build.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/env.sh"
cd "$NXO_ROOT"

ALL=(sdk libcxx openssl ffmpeg eden package)
CACHED=(sdk libcxx openssl ffmpeg)
stages=("$@")
[ "${#stages[@]}" -eq 0 ] && stages=("${ALL[@]}")

is_cached() { local s; for s in "${CACHED[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }

for s in "${stages[@]}"; do
    if [ ! -f "$HERE/stage-$s.sh" ]; then echo "::error::no such stage: $s"; exit 2; fi
    if is_cached "$s" && [ -f "stamps/$s" ]; then
        echo "stage $s: already built (from the cache), skipping"
        continue
    fi
    echo "::group::stage $s"
    start=$(date +%s)
    if bash "$HERE/stage-$s.sh" 2>&1 | tee "logs/$s.log"; then
        echo "::endgroup::"
        is_cached "$s" && touch "stamps/$s"
        summary "- stage **$s**: ok ($(( $(date +%s) - start )) s)"
    else
        echo "::endgroup::"
        echo "::error::stage '$s' failed; the last lines of its log follow (the whole log is in the build-diagnostics artifact)"
        tail -n 40 "logs/$s.log"
        summary "- stage **$s**: FAILED, last lines of its log:"
        summary '```'
        tail -n 40 "logs/$s.log" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
        summary '```'
        summary "- the first errors of stage **$s**:"
        summary '```'
        grep -m 25 -E "error:|Error |FAILED:|fatal:" "logs/$s.log" | cut -c1-300 >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
        summary '```'
        exit 1
    fi
done
