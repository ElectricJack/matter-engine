#!/usr/bin/env bash
# setup-worktree.sh — Restore directory symlinks (junctions) after git worktree creation.
#
# Git worktrees on Windows render tracked symlinks as small text files containing
# the target path. This script replaces them with NTFS junctions so the build
# system can follow them.
#
# Usage:  bash setup-worktree.sh          (run from repo root)
# Safe to re-run: existing junctions are skipped.

set -euo pipefail
cd "$(dirname "$0")"

fix_junction() {
    local link="$1" target="$2"
    # If it's already a directory (junction or real), skip.
    if [ -d "$link" ] && [ ! -f "$link" ]; then
        echo "  OK (already a directory): $link"
        return
    fi
    # Remove the text-file placeholder git created.
    rm -f "$link"
    # Create an NTFS junction (no admin required).
    #
    # PowerShell rather than `cmd //c mklink`: this repo's checkout path
    # contains spaces ("D:\Shared With Desktop\..."), and MSYS2 mangles the
    # nested quoting cmd needs. The old cmd form failed on the very FIRST
    # junction with "The system cannot find the path specified", and with an
    # absolute link path it failed differently ("The filename, directory name,
    # or volume label syntax is incorrect") -- so this script never worked here
    # and every worktree so far had its junctions made by hand.
    #
    # Both paths are made absolute and handed over through the ENVIRONMENT,
    # which sidesteps shell, cmd and PowerShell quoting all at once.
    ME_LINK="$(cygpath -w "$(pwd)/$link")" \
    ME_TARGET="$(cygpath -w "$target")" \
        powershell -NoProfile -NonInteractive -Command \
        'New-Item -ItemType Junction -Path $env:ME_LINK -Target $env:ME_TARGET | Out-Null'
    echo "  Created junction: $link -> $target"
}

echo "Setting up symlinks/junctions..."
# MatterEditor/shaders_gpu -> MatterEngine3/shaders_gpu used to be a third
# junction here. Both it and its target were retired with the GL path: Vulkan
# shader sources live in MatterEngine3/shaders_vk/ and reach the binaries as
# embedded SPIR-V, so nothing needs to see them through a second path.
fix_junction "MatterEngine3/shaders"      "$(pwd)/libs/MatterSurfaceLib/shaders"
fix_junction "MatterEditor/shaders"       "$(pwd)/libs/MatterSurfaceLib/shaders"
echo "Done."
