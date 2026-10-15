#!/bin/sh
# Run the game headless for SECONDS with an optional input script and save
# the last frame as PNG.
#   run.sh OUT_DIR SECONDS ["wait 5 click 640 480 ..."] [GAME_DIR]
# GAME_DIR defaults to ./data; the overlay (what the engine writes: saves,
# BGI.gdb) is kept in OUT_DIR/overlay so consecutive runs share it.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$1"; SECS="$2"; SCRIPT="$3"; GAME="${4:-$ROOT/data}"
mkdir -p "$OUT"
cd "$GAME"
BGI_MSGBOX_STDERR=1 BGI_WASMSIM_OVERLAY="$OUT/overlay" BGI_WASMSIM_SECONDS="$SECS" \
BGI_WASMSIM_SHOT="$OUT/shot.ppm" BGI_WASMSIM_SCRIPT="$SCRIPT" \
	"$ROOT/engine/openbgi/bin/openbgi-wasmsim" > "$OUT/log.txt" 2>&1 || true
sips -s format png "$OUT/shot.ppm" --out "$OUT/shot.png" > /dev/null 2>&1 || true
tail -15 "$OUT/log.txt"
