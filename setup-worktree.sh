#!/usr/bin/env bash
# setup-worktree.sh — historical worktree bootstrap step. No longer needed.
#
# This used to restore the MatterEngine3/shaders and MatterEditor/shaders
# directory symlinks as NTFS junctions after a Windows `git worktree add`
# rendered the tracked symlinks as small text-file placeholders (see
# CLAUDE.md's old "Worktree setup (symlinks)" section for the history).
#
# 2026-08-14: both symlinks were removed. MatterEngine3/Makefile and
# MatterEditor/Makefile now reference libs/MatterSurfaceLib/shaders directly,
# so there is nothing left for this script to fix up. Kept as a stub (rather
# than deleted) so old habits -- `bash setup-worktree.sh` out of muscle memory
# after creating a worktree -- fail loudly-but-kindly instead of silently
# doing nothing.

echo "setup-worktree.sh is no longer needed: shader symlinks were removed (2026-08-14); builds reference libs/MatterSurfaceLib/shaders directly."
exit 0
