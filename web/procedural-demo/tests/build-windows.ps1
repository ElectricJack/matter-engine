[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
Import-Module (Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1') -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot
$buildDirectory = Join-Path $PSScriptRoot 'build\windows-msvc\release'
$developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
$configure = '{0} && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_MAKE_PROGRAM="{4}" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF' -f $developerEnvironment, $toolchain.CMake, $PSScriptRoot, $buildDirectory, $toolchain.Ninja
& $env:ComSpec /d /s /c $configure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$build = '{0} && "{1}" --build "{2}" --target matter_demo_native_recipes --parallel 2' -f $developerEnvironment, $toolchain.CMake, $buildDirectory
& $env:ComSpec /d /s /c $build
exit $LASTEXITCODE
