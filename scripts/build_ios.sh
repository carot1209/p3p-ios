#!/bin/bash
# Builds the unsigned IPA on a macOS runner (GitHub Actions) or a Mac.
#   scripts/build_ios.sh
# Input : game/eboot.elf (decrypted ULUS-10512 executable, private)
# Output: out/P3P-unsigned.ipa, out/build-info.txt
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UPSTREAM_URL="https://github.com/endercodezz/P3P3DS.git"
UPSTREAM_REF="$(cat "$ROOT/upstream.ref")"
BUILD="$ROOT/build"
UP="$BUILD/upstream"
GEN="$BUILD/gen/auto"
OUT="$ROOT/out"
CACHE="${P3P_AOT_CACHE:-$BUILD/aot-cache}"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
BUILD_ID="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo dev)"

mkdir -p "$BUILD" "$OUT" "$CACHE" "$BUILD/gen"
ELF="$ROOT/game/eboot.elf"
[ -f "$ELF" ] || { echo "game/eboot.elf is missing"; exit 1; }
EXPECTED="be2abbd43a4ae7ce5aa2f1f146083ffe0d924fc2eb1874e6fcdc21cba40d49db"
ACTUAL="$(shasum -a 256 "$ELF" | cut -d' ' -f1)"
[ "$ACTUAL" = "$EXPECTED" ] || { echo "game/eboot.elf: SHA-256 $ACTUAL, expected $EXPECTED"; exit 1; }

echo "== upstream $UPSTREAM_REF"
if [ ! -d "$UP/.git" ]; then
    git init -q "$UP"
    git -C "$UP" remote add origin "$UPSTREAM_URL"
fi
if [ "$(git -C "$UP" rev-parse HEAD 2>/dev/null || true)" != "$UPSTREAM_REF" ]; then
    git -C "$UP" fetch -q --depth 1 origin "$UPSTREAM_REF"
    git -C "$UP" checkout -q --force FETCH_HEAD
fi
for patch in "$ROOT"/patches/*.patch; do
    [ -e "$patch" ] || continue
    if git -C "$UP" apply --check "$patch" 2>/dev/null; then
        echo "   patch $(basename "$patch")"
        git -C "$UP" apply "$patch"
    fi
done

echo "== code generator (host)"
RECOMP="$UP/recomp/PSPRecomp"
if [ ! -x "$BUILD/psp_recomp" ]; then
    c++ -std=c++20 -O2 -I"$RECOMP/include" "$RECOMP"/src/decoder.cpp "$RECOMP"/src/deflate.cpp "$RECOMP"/src/elf32.cpp \
        "$RECOMP"/src/guest_memory.cpp "$RECOMP"/src/nid_registry.cpp "$RECOMP"/src/program_analysis.cpp \
        "$RECOMP"/src/runtime.cpp "$RECOMP"/src/sha256.cpp "$RECOMP"/tools/codegen_main.cpp -o "$BUILD/psp_recomp"
fi

echo "== AOT generation"
rm -rf "$GEN"; mkdir -p "$GEN"
python3 "$UP/tools/cwcheat_patches.py" "$UP/p3p/p3p-patches/ULUS10512.ini" "$UP/profiles/p3p/config/cwcheat_patches.txt" "$BUILD/gen/patches.txt"
# Cutscenes Restored "Unhardcode FMV" (patches/fmv_unhardcode.txt; the patch
# list reader takes only "<address> <value> <width>" triples, so no comments).
grep '^0x' "$ROOT/patches/fmv_unhardcode.txt" >> "$BUILD/gen/patches.txt"
"$BUILD/psp_recomp" "$ELF" --auto "$GEN" 0x08804000 16384 "$BUILD/gen/patches.txt" --no-transfer-records | tail -8
python3 "$UP/tools/verify_aot_layout.py" "$GEN" 237 16384

echo "== ffmpeg (movie decoder)"
FFMPEG="${P3P_FFMPEG_PREFIX:-$BUILD/ffmpeg-ios}"
"$ROOT/scripts/build_ffmpeg.sh" ios "$FFMPEG"

# The AOT archive only changes with the EBOOT, the upstream revision and the
# compiler flags: reuse it when all three are the same.
KEY="$(cat "$ELF" "$ROOT/scripts/gen_make.py" "$ROOT/upstream.ref" "$ROOT/patches/fmv_unhardcode.txt" | shasum -a 256 | cut -c1-16)"
ARCHIVE="$CACHE/libp3p_aot-$KEY.a"
IOS="$BUILD/ios"
if [ -f "$ARCHIVE" ]; then
    echo "== AOT archive from cache ($KEY)"
    python3 "$ROOT/scripts/gen_make.py" --target ios --upstream "$UP" --gen "$GEN" --out "$IOS" --build-id "$BUILD_ID" --aot-archive "$ARCHIVE" --ffmpeg "$FFMPEG"
else
    echo "== AOT archive will be compiled ($KEY)"
    python3 "$ROOT/scripts/gen_make.py" --target ios --upstream "$UP" --gen "$GEN" --out "$IOS" --build-id "$BUILD_ID" --ffmpeg "$FFMPEG"
fi

echo "== compile ($JOBS jobs)"
make -C "$IOS" -j"$JOBS" 2>&1 | { grep -v '^CXX generated_unit_' || true; }
[ -x "$IOS/P3P" ] || { echo "no executable"; exit 1; }
[ -f "$ARCHIVE" ] || cp "$IOS/libp3p_aot.a" "$ARCHIVE"

echo "== bundle"
APP="$BUILD/Payload/P3P.app"
rm -rf "$BUILD/Payload"; mkdir -p "$APP"
cp "$IOS/P3P" "$APP/P3P"
strip -x "$APP/P3P" 2>/dev/null || true
cp "$ROOT/ios/Info.plist" "$APP/Info.plist"
cp "$ELF" "$APP/eboot.elf"
cp "$ROOT"/ios/icons/*.png "$APP/"
printf 'APPL????' > "$APP/PkgInfo"
( cd "$BUILD" && rm -f "$OUT/P3P-unsigned.ipa" && zip -qry "$OUT/P3P-unsigned.ipa" Payload )

{
    echo "build    : $BUILD_ID"
    echo "upstream : $UPSTREAM_REF"
    echo "date     : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    xcodebuild -version 2>/dev/null | tr '\n' ' '; echo
    echo "ipa      : $(stat -f %z "$OUT/P3P-unsigned.ipa" 2>/dev/null || stat -c %s "$OUT/P3P-unsigned.ipa") bytes"
    file "$APP/P3P"
    otool -L "$APP/P3P" 2>/dev/null | head -20 || true
    ls -la "$APP"
} > "$OUT/build-info.txt"
cat "$OUT/build-info.txt"
