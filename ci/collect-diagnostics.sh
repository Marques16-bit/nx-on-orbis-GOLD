# Gathers what is needed to understand a failed (or odd) build into diagnostics/.
cd "${NXO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}" || exit 0
rm -rf diagnostics; mkdir -p diagnostics
cp -r logs diagnostics/ 2>/dev/null
for f in build-libcxx/CMakeFiles/CMakeError.log build-libcxx/CMakeFiles/CMakeOutput.log \
         build-eden/CMakeFiles/CMakeError.log build-eden/CMakeFiles/CMakeOutput.log \
         build-eden/eden-ps4-build-id.txt build-eden/.ninja_log \
         deps/build-ffmpeg/ffbuild/config.log; do
    [ -f "$f" ] && cp --parents "$f" diagnostics/
done
ls stamps > diagnostics/stamps.txt 2>/dev/null
exit 0
