[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',
    [switch]$Test,
    [switch]$Matrix,
    [switch]$Solver
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
Import-Module (Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1') -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot
$developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
$testing = if ($Test -or $Matrix) { 'ON' } else { 'OFF' }
$modes = if ($Matrix) { @('ON', 'OFF') } elseif ($Solver) { @('ON') } else { @('OFF') }
foreach ($mode in $modes) {
    $buildDirectory = Join-Path $PSScriptRoot "build\windows-msvc\$($Config.ToLowerInvariant())-$($mode.ToLowerInvariant())"
    $configure = '{0} && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_MAKE_PROGRAM="{4}" -DCMAKE_BUILD_TYPE={5} -DBUILD_TESTING={6} -DMATTER_PHYSICS_BOX3D={7}' -f $developerEnvironment, $toolchain.CMake, $PSScriptRoot, $buildDirectory, $toolchain.Ninja, $Config, $testing, $mode
    & $env:ComSpec /d /s /c $configure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $build = '{0} && "{1}" --build "{2}" --parallel 2' -f $developerEnvironment, $toolchain.CMake, $buildDirectory
    & $env:ComSpec /d /s /c $build
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if ($testing -eq 'ON') {
        $ctest = Join-Path (Split-Path $toolchain.CMake) 'ctest.exe'
        & $ctest --test-dir $buildDirectory --output-on-failure -j 1
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}
if ($Matrix) {
    $root = Join-Path $PSScriptRoot 'build\windows-msvc'
    $reader = Join-Path $root "$($Config.ToLowerInvariant())-off\matter_collision_tests.exe"
    $artifact = Join-Path $root "$($Config.ToLowerInvariant())-on\settled.asset"
    & $reader read $artifact
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $ninja = Get-Content -Raw (Join-Path $root "$($Config.ToLowerInvariant())-off\build.ninja")
    if ($ninja -match 'third_party[/\\]box3d|tileset_settle\.cpp|flecs\.c|quickjs\.c|vk_scene_renderer\.cpp') {
        throw 'Solver-off graph contains a forbidden dependency'
    }
    Write-Output 'PHYS-INDEPENDENCE: solver-off graph and cross-build baked loading passed'
}
exit 0
