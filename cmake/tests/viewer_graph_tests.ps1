[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Import-Module (Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1') -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot
$graphTest = Join-Path $repositoryRoot 'cmake\tests\viewer_graph_tests.cmake'
$developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f `
    $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
$command = '{0} && "{1}" -DMATTER_TEST_PYTHON:FILEPATH="{2}" -P "{3}"' -f `
    $developerEnvironment, $toolchain.CMake, $toolchain.Python, $graphTest
& $env:ComSpec /d /s /c $command
exit $LASTEXITCODE
