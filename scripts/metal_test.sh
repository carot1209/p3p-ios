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
        -framework Metal -framework Foundation -framework CoreGraphics -o "$OUT/gpu_replay" || exit 1
    echo "== replay"
    "$OUT/gpu_replay" "$OUT/trace.bin" "$OUT" soft metal
    echo "exit code: $?"
} 2>&1 | tee "$OUT/result.txt"
for f in "$OUT"/*.bmp; do
    [ -e "$f" ] || continue
    sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null 2>&1 && rm -f "$f"
done
rm -f "$OUT/trace.bin" "$OUT/gpu_replay"
ls "$OUT" | head -40
