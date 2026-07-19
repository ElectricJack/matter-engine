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
    cmd //c "mklink /J \"$(cygpath -w "$link")\" \"$(cygpath -w "$target")\"" > /dev/null
    echo "  Created junction: $link -> $target"
}

echo "Setting up symlinks/junctions..."
fix_junction "MatterEngine3/shaders"      "$(pwd)/MatterSurfaceLib/shaders"
fix_junction "MatterViewer/shaders"       "$(pwd)/MatterSurfaceLib/shaders"
fix_junction "MatterViewer/shaders_gpu"   "$(pwd)/MatterEngine3/shaders_gpu"
echo "Done."
