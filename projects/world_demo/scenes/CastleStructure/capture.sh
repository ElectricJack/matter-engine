#!/usr/bin/env bash
# Native raster + native_rt captures of the CastleStructure fixture from WSL.
# Usage (repo root, after tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor):
#   bash projects/world_demo/scenes/CastleStructure/capture.sh [C:/tmp/castle-structure]
# The editor splits `shot` paths at spaces, so the output directory must be a
# space-free Windows path. bake.finished fires before parts finish publishing,
# so the shot timeline (capture_timeline.txt) is appended to the append-only
# MATTER_CMD_FIFO only once the "bake N/M" part counter reaches M and the log
# has been quiet for 30 s. Every shot's PNG and .done sidecar is verified.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
OUT_WIN="${1:-C:/tmp/castle-structure}"
OUT="$(wslpath -u "$OUT_WIN")"
mkdir -p "$OUT"
CMD="$OUT/cmd.txt" LOG="$OUT/editor.log"
sed "s|@OUT@|$OUT_WIN|" "$HERE/capture_timeline.txt" > "$OUT/timeline.txt"
grep '^shot ' "$OUT/timeline.txt" | while read -r _ p; do rm -f "$OUT/$(basename "$p")" "$OUT/$(basename "$p").done"; done
printf 'wait_event bake.finished 2400\n' > "$CMD"
cd "$REPO/MatterEditor"
TMPW="$(cmd.exe /c 'echo %TEMP%' 2>/dev/null | tr -d '\r')"
WSLENV=MATTER_WORLD:MATTER_CMD_FIFO:MATTER_HIDE_UI:TMP:TEMP \
MATTER_WORLD=CastleStructure MATTER_CMD_FIFO="$(wslpath -m "$CMD")" MATTER_HIDE_UI=1 TMP="$TMPW" TEMP="$TMPW" \
timeout 5400 ./build/windows-msvc/editor.exe > "$LOG" 2>&1 &
EPID=$!
last=0; quiet=0
while kill -0 $EPID 2>/dev/null; do
  prog=$(grep -o 'bake [0-9]*/[1-9][0-9]*' "$LOG" | tail -1); n=${prog#bake }
  size=$(stat -c %s "$LOG"); if [ "$size" = "$last" ]; then quiet=$((quiet+10)); else quiet=0; last=$size; fi
  [ -n "$prog" ] && [ "${n%/*}" = "${n#*/}" ] && [ $quiet -ge 30 ] && break
  sleep 10
done
{ echo 'wait_frames 90'; cat "$OUT/timeline.txt"; } >> "$CMD"
wait $EPID; echo "editor exit=$?"
status=0
grep '^shot ' "$OUT/timeline.txt" | while read -r _ p; do
  f="$OUT/$(basename "$p")"; if [ -s "$f" ] && [ -e "$f.done" ]; then echo "ok   $(basename "$f")"; else echo "MISS $(basename "$f")"; fi
done
echo "vulkan validation lines: $(grep -c -i -E 'validation|VUID' "$LOG")"
