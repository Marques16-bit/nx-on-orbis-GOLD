# FFmpeg (h264, vp8, vp9 decoders) for the PS4 -> prefix/ (docs/BUILDING.md, section 4).
set -euo pipefail
cd "$NXO_ROOT"
source tools/env-build.sh
FFMPEG_COMMIT=c7b5f1537d
mkdir -p deps
if [ ! -f "prefix/lib/libavcodec.a" ]; then
    rm -rf deps/ffmpeg deps/build-ffmpeg
    git clone --filter=blob:none --no-checkout https://github.com/FFmpeg/FFmpeg.git deps/ffmpeg
    git -C deps/ffmpeg checkout --detach "$FFMPEG_COMMIT"
    mkdir -p deps/build-ffmpeg/tmp
    cd deps/build-ffmpeg
    export TMPDIR="$PWD/tmp"      # configure needs a TMPDIR it can write to
    # --host-cc is the native (Linux) clang; the target compiler is the PS4 wrapper.
    ../ffmpeg/configure --prefix="$NXO_ROOT/prefix" --enable-cross-compile --arch=x86_64 --target-os=freebsd \
        --host-cc=clang --host-ld=clang \
        --cc="bash $NXO_ROOT/tools/ps4-cc.sh" --ld="bash $NXO_ROOT/tools/ps4-cc.sh" \
        --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm \
        --disable-x86asm --disable-autodetect --disable-everything --disable-programs --disable-doc \
        --disable-avdevice --disable-avformat --disable-network --disable-swresample \
        --enable-decoder='h264,vp8,vp9' --enable-filter='yadif,scale' --enable-pic --enable-pthreads \
        --disable-shared --enable-static
    make -j"$JOBS"
    make install
    cd "$NXO_ROOT"
    cp deps/build-ffmpeg/ffbuild/config.log logs/ffmpeg-config.log 2>/dev/null || true
    rm -rf deps/ffmpeg deps/build-ffmpeg
fi
ls -la prefix/lib/libavcodec.a
