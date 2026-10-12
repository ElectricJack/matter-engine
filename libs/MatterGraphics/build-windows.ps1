[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',
    [switch]$Test,
    [switch]$NativeWaterTemporal
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Import-Module (Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1') -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot
$buildDirectory = Join-Path $PSScriptRoot "build\windows-msvc\$($Config.ToLowerInvariant())"
$sourceDirectory = $PSScriptRoot
if ($NativeWaterTemporal) {
    $sourceDirectory = Join-Path $PSScriptRoot 'src\native'
    $buildDirectory = Join-Path $PSScriptRoot "build\native-wt\$($Config.ToLowerInvariant())"
}
$developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
$env:VULKAN_SDK = $toolchain.VulkanSdk
$env:VK_LAYER_PATH = Join-Path $toolchain.VulkanSdk 'Bin'
$env:MATTER_GRAPHICS_REQUIRE_VALIDATION = '1'
$configure = '{0} && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_MAKE_PROGRAM="{4}" -DCMAKE_BUILD_TYPE={5} -DBUILD_TESTING=ON -DMATTER_GRAPHICS_VULKAN=ON -DPython3_EXECUTABLE="{6}"' -f $developerEnvironment, $toolchain.CMake, $sourceDirectory, $buildDirectory, $toolchain.Ninja, $Config, $toolchain.Python
if ($NativeWaterTemporal) {
    $configure = '{0} && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_MAKE_PROGRAM="{4}" -DCMAKE_BUILD_TYPE={5} -DBUILD_TESTING=ON' -f $developerEnvironment, $toolchain.CMake, $sourceDirectory, $buildDirectory, $toolchain.Ninja, $Config
}
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
