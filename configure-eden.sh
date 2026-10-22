#!/usr/bin/env bash
# Configures Eden (deps/eden) for the PS4 into build-eden/.
set -e
# NXO_ROOT set (CI / Linux): the repository root. Unset: the original Windows machine.
if [ -n "${NXO_ROOT:-}" ]; then R="$NXO_ROOT"; W="$NXO_ROOT"; else R=/c/Users/alejo/eden-ps4; W=C:/Users/alejo/eden-ps4; fi
source "$R/tools/env-build.sh"
unset OO_PS4_TOOLCHAIN
cmake -S $R/deps/eden -B $R/build-eden -G Ninja \
 -DCMAKE_TOOLCHAIN_FILE=$W/toolchain/eden-ps4.cmake -DPS4_BUILD_PKG=OFF \
 -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=OFF -DENABLE_QT=OFF -DYUZU_CMD=OFF -DYUZU_ROOM=OFF \
 -DYUZU_ROOM_STANDALONE=OFF -DYUZU_TESTS=OFF -DBUILD_TESTING=OFF -DENABLE_OPENGL=OFF -DENABLE_CUBEB=OFF \
 -DENABLE_WEB_SERVICE=OFF -DENABLE_LIBUSB=OFF -DYUZU_CRASH_DUMPS=OFF -DENABLE_WERROR=OFF \
 -Dzstd_FORCE_BUNDLED=ON -DBoost_FORCE_BUNDLED=ON -Dfmt_FORCE_BUNDLED=ON -DDYNARMIC_ENABLE_NO_EXECUTE_SUPPORT=OFF \
 -DYUZU_USE_EXTERNAL_FFMPEG=OFF -DYUZU_DOWNLOAD_TIME_ZONE_DATA=ON \
 -DYUZU_USE_BUNDLED_OPENSSL=OFF -DOpenSSL_FORCE_SYSTEM=ON -DOPENSSL_ROOT_DIR=$PS4_PREFIX \
 -DOPENSSL_INCLUDE_DIR=$PS4_PREFIX/include -DOPENSSL_SSL_LIBRARY=$PS4_PREFIX/lib/libssl.a \
 -DOPENSSL_CRYPTO_LIBRARY=$PS4_PREFIX/lib/libcrypto.a \
 -U "FFmpeg_*" -DFFMPEG_DIR=$PS4_PREFIX \
 -DCMAKE_PROJECT_yuzu_INCLUDE=$W/frontend/inject.cmake "$@"
