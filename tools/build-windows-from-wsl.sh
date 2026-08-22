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
powershell='/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe'

args=(-NoProfile -ExecutionPolicy Bypass -File "${windows_root}\\tools\\build-windows.ps1" -Config "$config")
if [[ "$target" == 'preflight' ]]; then
    args+=(-PreflightOnly)
elif [[ -n "$target" ]]; then
    args+=(-Target "$target")
fi

"$powershell" "${args[@]}"

windows_artifact="${windows_root}\\MatterEditor\\build\\windows-msvc\\editor.exe"
wsl_artifact="${repository_root}/MatterEditor/build/windows-msvc/editor.exe"
printf 'MATTER_WINDOWS_ARTIFACT=%s\n' "$windows_artifact"
printf 'MATTER_WSL_ARTIFACT=%s\n' "$wsl_artifact"
