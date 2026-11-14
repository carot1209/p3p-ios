#!/bin/bash
# Minimal static FFmpeg (h264 + atrac3p decoders only) for the movie decoder.
#   scripts/build_ffmpeg.sh ios|host <prefix>
# Skips the build when <prefix>/lib/libavcodec.a already exists (CI cache).
set -euo pipefail
TARGET="$1"; PREFIX="$2"
VERSION="n7.1"
[ -f "$PREFIX/lib/libavcodec.a" ] && { echo "ffmpeg: cached in $PREFIX"; exit 0; }
SRC="$(mktemp -d)/ffmpeg"
git clone -q --depth 1 --branch "$VERSION" https://github.com/FFmpeg/FFmpeg.git "$SRC"
COMMON=(--prefix="$PREFIX" --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect
        --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-avfilter
        --enable-decoder=h264,atrac3p --enable-parser=h264 --enable-static --disable-shared --enable-pic)
cd "$SRC"
if [ "$TARGET" = "ios" ]; then
    SDK="$(xcrun -sdk iphoneos --show-sdk-path)"
    CC="$(xcrun -sdk iphoneos -f clang)"
    FLAGS="-arch arm64 -isysroot $SDK -miphoneos-version-min=15.0"
    ./configure "${COMMON[@]}" --enable-cross-compile --target-os=darwin --arch=aarch64 --cc="$CC" \
        --extra-cflags="$FLAGS" --extra-ldflags="$FLAGS" > "$PREFIX.configure.log" 2>&1 || { tail -40 "$PREFIX.configure.log"; tail -60 ffbuild/config.log; exit 1; }
else
    ./configure "${COMMON[@]}" > "$PREFIX.configure.log" 2>&1 || { tail -40 "$PREFIX.configure.log"; exit 1; }
fi
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" > /dev/null
make install > /dev/null
echo "ffmpeg $VERSION ($TARGET) -> $PREFIX"
ls -la "$PREFIX/lib"
