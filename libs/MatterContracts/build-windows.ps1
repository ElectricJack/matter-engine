[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',
    [switch]$Test,
    [switch]$Matrix,
    # Optional native venv for test-only jsonschema; compiler/toolchain still
    # comes exclusively from the supported repository resolver.
    [string]$TestPython
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Import-Module (Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1') -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot
if (-not $TestPython) { $TestPython = $toolchain.Python }
if (-not (Test-Path -LiteralPath $TestPython -PathType Leaf)) { throw 'TestPython must name an existing native Python interpreter.' }
$buildDirectory = Join-Path $PSScriptRoot "build\windows-msvc\$($Config.ToLowerInvariant())"
$developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
$configure = '{0} && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_MAKE_PROGRAM="{4}" -DCMAKE_BUILD_TYPE={5} -DBUILD_TESTING=ON -DPython3_EXECUTABLE="{6}"' -f $developerEnvironment, $toolchain.CMake, $PSScriptRoot, $buildDirectory, $toolchain.Ninja, $Config, $TestPython
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
if ($Matrix) {
    $matrixScript = Join-Path $PSScriptRoot 'tests\build_matrix.py'
    $componentMatrixCommand = '{0} && "{1}" "{2}" --cmake "{3}" --generator Ninja --make-program "{4}"' -f $developerEnvironment, $toolchain.Python, $matrixScript, $toolchain.CMake, $toolchain.Ninja
    & $env:ComSpec /d /s /c $componentMatrixCommand
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
exit 0
