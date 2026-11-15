#!/bin/bash
# Tests the Metal back end on a Mac (GitHub's macOS runners have a GPU):
# replays tests/gpu_trace.bin.gz -- frames of the real game recorded with
# runner/host_test --trace -- on the CPU reference back end and on Metal,
# writes both sets of pictures as PNG and how much they differ.
#   scripts/metal_test.sh        -> out/metal/result.txt, out/metal/*.png
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/out/metal"
rm -rf "$OUT"; mkdir -p "$OUT"
{
    sw_vers 2>/dev/null | tr '\n' ' '; echo
    gunzip -c "$ROOT/tests/gpu_trace.bin.gz" > "$OUT/trace.bin" || exit 1
    echo "== build"
    xcrun clang++ -std=c++20 -O2 -fobjc-arc -DP3P_HAVE_METAL -I"$ROOT/gpu" \
        "$ROOT/gpu/replay_main.cpp" "$ROOT/gpu/trace.cpp" "$ROOT/gpu/soft_backend.cpp" "$ROOT/gpu/metal_backend.mm" \
        -framework Metal -framework QuartzCore -framework Foundation -framework CoreGraphics -o "$OUT/gpu_replay" || exit 1
    echo "== replay"
    "$OUT/gpu_replay" "$OUT/trace.bin" "$OUT" soft metal
    echo "exit code: $?"
    echo "== replay at 3x (read back as block means; edges differ a little from 1x)"
    "$OUT/gpu_replay" "$OUT/trace.bin" "$OUT" soft metal3
    echo "exit code: $?"
    echo "== movie pictures (uploads into a target, then shown) at 1x and 4x"
    gunzip -c "$ROOT/tests/gpu_trace_movie.bin.gz" > "$OUT/movie.bin" || exit 1
    mkdir -p "$OUT/movie1" "$OUT/movie4"
    "$OUT/gpu_replay" "$OUT/movie.bin" "$OUT/movie1" soft metal
    echo "exit code: $?"
    "$OUT/gpu_replay" "$OUT/movie.bin" "$OUT/movie4" soft metal4
    echo "exit code: $?"
    for d in movie1 movie4; do for f in "$OUT/$d"/*.bmp; do mv "$f" "$OUT/${d}_$(basename "$f")"; done; rmdir "$OUT/$d"; done
    rm -f "$OUT/movie.bin"
} 2>&1 | tee "$OUT/result.txt"
for f in "$OUT"/*.bmp; do
    [ -e "$f" ] || continue
    sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null 2>&1 && rm -f "$f"
done
rm -f "$OUT/trace.bin" "$OUT/gpu_replay"
ls "$OUT" | head -40
