#!/usr/bin/env bash
# package_explorer.sh — stage the ExplorerDemo distributable zip.
#
# Usage: Run from the repo root or ExplorerDemo/.
#   bash ExplorerDemo/tools/package_explorer.sh
#
# Produces: dist/MeadowValley-Explorer-<YYYYMMDD>.zip
#   explorer.exe         — statically linked Windows executable
#   WorldData/
#     schemas/           — world schema .js files (all worlds, Meadow incl.)
#     shared-lib/        — shared DSL library .js files
#     worlds/Meadow/     — Meadow world data (world.manifest + cache can be warm-started)
#   README.txt           — controls, sysreq, build info
#
# The zip is self-contained: double-click explorer.exe on Windows 10+ (GL 4.6 GPU).
# First run bakes Meadow (~2 min, CPU+GPU); subsequent runs reuse the cache/ dir
# created next to explorer.exe.
#
# Requirements (build host):
#   - x86_64-w64-mingw32-g++-posix (mingw-w64 cross toolchain)
#   - zip

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXPLORER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$EXPLORER_DIR/.." && pwd)"

ME3_DIR="$REPO_ROOT/MatterEngine3"
DIST_DIR="$EXPLORER_DIR/dist"
DATE_STAMP="$(date +%Y%m%d)"
ZIP_NAME="MeadowValley-Explorer-$DATE_STAMP.zip"
STAGE_DIR="$(mktemp -d /tmp/explorer_stage_XXXXXX)"

cleanup() {
    rm -rf "$STAGE_DIR"
}
trap cleanup EXIT

echo "==> Building explorer.exe (Windows cross-compile)..."
make -C "$EXPLORER_DIR" windows

EXE="$EXPLORER_DIR/explorer.exe"
if [ ! -f "$EXE" ]; then
    echo "ERROR: explorer.exe not found after build (expected: $EXE)"
    exit 1
fi

echo "==> Staging package layout in $STAGE_DIR..."

# --- explorer.exe ---
cp "$EXE" "$STAGE_DIR/explorer.exe"

# --- WorldData/ layout ---
mkdir -p "$STAGE_DIR/WorldData/schemas"
mkdir -p "$STAGE_DIR/WorldData/worlds"
mkdir -p "$STAGE_DIR/WorldData/shared-lib"

# Copy all world schemas (Meadow.js + supporting part schemas).
cp -r "$ME3_DIR/examples/world_demo/schemas/." "$STAGE_DIR/WorldData/schemas/"

# Copy the Meadow world data (world.manifest; no bake cache — user generates it).
cp -r "$ME3_DIR/examples/world_demo/WorldData/Meadow" "$STAGE_DIR/WorldData/worlds/"

# Copy shared DSL library files.
cp -r "$ME3_DIR/shared-lib/." "$STAGE_DIR/WorldData/shared-lib/"

# --- README.txt ---
cat > "$STAGE_DIR/README.txt" << 'EOF'
MeadowValley Explorer
=====================

Fly through the Meadow Valley procedural world, generated entirely on your
machine from the .js schema files in WorldData/.

System requirements
-------------------
  OS:  Windows 10 or later (64-bit)
  GPU: OpenGL 4.6 required. An NVIDIA RTX 3060-class GPU or better is
       recommended for smooth 60 fps. Integrated graphics may work but will
       be slower.

Running
-------
  Double-click explorer.exe to launch.

  First run: Meadow Valley bakes for approximately 2 minutes (all CPU cores
  are used). Subsequent runs reuse the cache/ folder created next to explorer.exe
  and load in seconds.

Controls
--------
  Tab              Toggle mouse capture (required before mouselook)
  W / A / S / D   Move forward / left / back / right
  Q / E            Move down / up
  Left Shift       Speed boost (4x)
  Mouse (captured) Look around

  Gamepad:
    Left stick     Move
    Right stick    Look
    RT / LT        Move up / down (right trigger = up, left trigger = down)

About
-----
  Everything you see is computed on your machine from the .js files in
  WorldData/schemas/ and WorldData/shared-lib/ — there are no pre-built
  geometry assets. Read the scripts to understand how the world is defined,
  or modify them and delete cache/ to see the changes.

  Built with MatterEngine3, raylib, and QuickJS-ng.
EOF

# --- Zip ---
mkdir -p "$DIST_DIR"
ZIP_PATH="$DIST_DIR/$ZIP_NAME"
rm -f "$ZIP_PATH"

echo "==> Creating $ZIP_PATH ..."
# Use 'zip' if available; fall back to python3 -m zipfile (always present).
if command -v zip &>/dev/null; then
    (cd "$STAGE_DIR" && zip -r "$ZIP_PATH" .)
else
    python3 -c "
import zipfile, os, sys
stage = sys.argv[1]; out = sys.argv[2]
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk(stage):
        for f in files:
            fp = os.path.join(root, f)
            arcname = os.path.relpath(fp, stage)
            zf.write(fp, arcname)
print('zip written')
" "$STAGE_DIR" "$ZIP_PATH"
fi

SIZE_BYTES=$(stat -c %s "$ZIP_PATH" 2>/dev/null || stat -f %z "$ZIP_PATH")
SIZE_MB=$(echo "scale=1; $SIZE_BYTES / 1048576" | bc)

echo ""
echo "==> Done: $ZIP_PATH"
echo "    Size: ${SIZE_MB} MB (${SIZE_BYTES} bytes)"
echo ""
echo "    Contents:"
(cd "$STAGE_DIR" && find . -type f | sort | sed 's/^\.\//    /')
