[CmdletBinding()]
param([ValidateSet('RelWithDebInfo')][string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Import-Module (Join-Path $repo 'tools/windows/MatterWindowsToolchain.psm1') -Force
$tc = Resolve-MatterWindowsToolchain -RepositoryRoot $repo
$buildDirectory = Join-Path $PSScriptRoot "build/windows-msvc-$($Config.ToLowerInvariant())"
$dev = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f $tc.VsDevCmd, $tc.WindowsSdkVersion, $tc.MsvcToolsVersion
$configure = '{0} && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_MAKE_PROGRAM="{4}" -DCMAKE_BUILD_TYPE={5}' -f $dev, $tc.CMake, $PSScriptRoot, $buildDirectory, $tc.Ninja, $Config
& $env:ComSpec /d /s /c $configure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$build = '{0} && "{1}" --build "{2}"' -f $dev, $tc.CMake, $buildDirectory
& $env:ComSpec /d /s /c $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path (Split-Path $tc.CMake) 'ctest.exe') --test-dir $buildDirectory --output-on-failure -j 1
exit $LASTEXITCODE
