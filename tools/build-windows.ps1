[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',
    [string]$Target,
    [switch]$PreflightOnly
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$modulePath = Join-Path $PSScriptRoot 'windows\MatterWindowsToolchain.psm1'
Import-Module $modulePath -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot

if ($PreflightOnly) {
    $toolchain | ConvertTo-Json -Depth 2
    exit 0
}

$preset = "windows-msvc-$($Config.ToLowerInvariant())"
$configure = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} && "{2}" --preset "{3}"' -f $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.CMake, $preset
& $env:ComSpec /d /s /c $configure
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$build = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} && "{2}" --build --preset "{3}"' -f $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.CMake, $preset
if ($Target) {
    $build += ' --target "{0}"' -f $Target
}
& $env:ComSpec /d /s /c $build
exit $LASTEXITCODE
