[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Import-Module (Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1') -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot
$buildDirectory = Join-Path $repositoryRoot "MatterEngine3\build\terrain-coordinator\$($Config.ToLowerInvariant())"
$developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
$configure = '{0} && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_MAKE_PROGRAM="{4}" -DCMAKE_BUILD_TYPE={5} -DBUILD_TESTING=ON' -f $developerEnvironment, $toolchain.CMake, $PSScriptRoot, $buildDirectory, $toolchain.Ninja, $Config
& $env:ComSpec /d /s /c $configure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$build = '{0} && "{1}" --build "{2}" --parallel 2' -f $developerEnvironment, $toolchain.CMake, $buildDirectory
& $env:ComSpec /d /s /c $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($Test) {
    $ctest = Join-Path (Split-Path $toolchain.CMake) 'ctest.exe'
    & $ctest --test-dir $buildDirectory --output-on-failure -j 1
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
exit 0
