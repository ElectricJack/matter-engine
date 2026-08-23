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
$developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
$configure = '{0} && "{1}" --preset "{2}" -DCMAKE_MAKE_PROGRAM="{3}" -DMATTER_PYTHON_EXECUTABLE:FILEPATH="{4}"' -f $developerEnvironment, $toolchain.CMake, $preset, $toolchain.Ninja, $toolchain.Python
& $env:ComSpec /d /s /c $configure
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$build = '{0} && "{1}" --build --preset "{2}"' -f $developerEnvironment, $toolchain.CMake, $preset
if ($Target) {
    $build += ' --target "{0}"' -f $Target
}
& $env:ComSpec /d /s /c $build
$buildExitCode = $LASTEXITCODE
if ($buildExitCode -eq 0 -and
        ((-not $Target) -or $Target -in @('matter_editor', 'editor', 'all'))) {
    $artifact = Join-Path $repositoryRoot 'MatterEditor\build\windows-msvc\editor.exe'
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        $targetLabel = if ($Target) { $Target } else { '<default>' }
        Write-Error "editor-producing target '$targetLabel' succeeded but $artifact was not found"
        exit 1
    }
    Write-Output "MATTER_WINDOWS_ARTIFACT=$artifact"
}
exit $buildExitCode
