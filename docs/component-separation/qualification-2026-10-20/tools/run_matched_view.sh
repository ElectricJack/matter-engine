#!/bin/bash
# usage: run_matched_view.sh <absolute rundir>  (env WORLD, default PortableCrateFixture)
# Legacy debug views (lit/albedo/normal/depth) via the FIFO plus the portable capture.
R=$1; mkdir -p $R; : > $R/cmd.txt
cd "$(dirname "$0")/../../../../MatterEditor"
DISPLAY=:0 MATTER_DISABLE_VK_RT=1 MATTER_VK_TEST_FORCE_RT_UNAVAILABLE=1 MATTER_VOLUMETRICS=0 MATTER_HIDE_WINDOW=1 MATTER_HIDE_UI=1 MATTER_WINDOW_WIDTH=480 MATTER_WINDOW_HEIGHT=270 MATTER_WORLD=${WORLD:-PortableCrateFixture} MATTER_CACHE_ROOT=$R/cache MATTER_PORTABLE_SCENE_CAPTURE=$R/portable MATTER_CMD_FIFO=$R/cmd.txt timeout 900 ./build/linux/editor > $R/run.log 2>&1 &
PID=$!; date +%s > $R/start
sleep 3
cat >> $R/cmd.txt <<FIFO
wait_idle 5 600
wait_frames 60
shot $R/lit.png
set viewer.debug.debug_view_mode 4
wait_frames 10
shot $R/legacy_albedo.png
set viewer.debug.debug_view_mode 1
wait_frames 10
shot $R/legacy_normal.png
set viewer.debug.debug_view_mode 2
wait_frames 10
shot $R/legacy_depth.png
quit
FIFO
wait $PID; echo exit=$?
