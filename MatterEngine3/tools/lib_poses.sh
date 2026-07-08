#!/usr/bin/env bash
# lib_poses.sh — shared camera pose set for scripted viewer runs.
# Source this file; do NOT execute it directly.
#
# Provides:
#   POSES_MEADOW  — 5-pose standard meadow set (0..256 world, same across
#                   viewer_shots.sh / meadow_sweep.sh / stress_sweep.sh).
#
# Usage:
#   source "$(dirname "$0")/lib_poses.sh"
#   for pose in "${POSES_MEADOW[@]}"; do  ...  done
#
# Each entry is a space-separated string: "NAME px py pz tx ty tz"

POSES_MEADOW=(
    "aerial    128 260 -40   128 0 128"     # whole scene in frustum
    "corner      8   2   8    60 1  60"     # in-crowd ground view
    "midfield   40   6  40   128 2 128"     # mid-distance, grass tri-share peak
    "far         4   3   4   250 0 250"     # far ground view across the world
    "empty     -40   5 -40  -200 5 -200"    # outside, looking away: fixed CPU cost
)
