#!/usr/bin/env bash
set -euo pipefail

if [[ ! -e /proc/sys/fs/binfmt_misc/WSLInterop ]]; then
    echo 'WSL interop is disabled: /proc/sys/fs/binfmt_misc/WSLInterop was not found.' >&2
    exit 1
fi

config="${1:-RelWithDebInfo}"
target="${2:-}"
repository_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
windows_root="$(wslpath -w "$repository_root")"
powershell="${MATTER_WINDOWS_POWERSHELL:-/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe}"

args=(-NoProfile -ExecutionPolicy Bypass -File "${windows_root}\\tools\\build-windows.ps1" -Config "$config")
if [[ "$target" == 'preflight' ]]; then
    args+=(-PreflightOnly)
elif [[ -n "$target" ]]; then
    args+=(-Target "$target")
fi

"$powershell" "${args[@]}"

if [[ "$target" == 'preflight' ]]; then
    exit 0
fi

if [[ "$target" == 'matter_dist' ]]; then
    wsl_package="${repository_root}/MatterEditor/build/dist/world_demo"
    for required in editor.exe build_features.json; do
        if [[ ! -f "$wsl_package/$required" ]]; then
            echo "matter_dist succeeded but verified package file was not found: ${wsl_package}/${required}" >&2
            exit 1
        fi
    done
    printf 'MATTER_WSL_PACKAGE=%s\n' "$wsl_package"
    exit 0
fi

if [[ -n "$target" && "$target" != 'matter_editor' &&
      "$target" != 'editor' && "$target" != 'all' ]]; then
    exit 0
fi

wsl_artifact="${repository_root}/MatterEditor/build/windows-msvc/editor.exe"
if [[ ! -f "$wsl_artifact" ]]; then
    echo "editor-producing target '${target:-<default>}' succeeded but ${wsl_artifact} was not found" >&2
    exit 1
fi
printf 'MATTER_WSL_ARTIFACT=%s\n' "$wsl_artifact"
