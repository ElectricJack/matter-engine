[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Import-Module (Join-Path $repo 'tools/windows/MatterWindowsToolchain.psm1') -Force
$tc = Resolve-MatterWindowsToolchain -RepositoryRoot $repo
Push-Location $repo
try {
    & (Join-Path $repo 'tools/build-windows.ps1') -Config RelWithDebInfo -Target part_graph_tests
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $buildDirectory = Join-Path $repo 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo'
    $dev = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f $tc.VsDevCmd, $tc.WindowsSdkVersion, $tc.MsvcToolsVersion
    $build = '{0} && "{1}" --build "{2}" --target terrain_mesher_tests obj_export_golden_tests solid_source_evaluation_tests shader_source_tests' -f $dev, $tc.CMake, $buildDirectory
    & $env:ComSpec /d /s /c $build
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Remove-Item Env:MATTER_EXPORT_GOLDEN_UPDATE -ErrorAction SilentlyContinue
    & (Join-Path (Split-Path $tc.CMake) 'ctest.exe') --test-dir $buildDirectory --output-on-failure -j 1 -R '^(part_graph_tests|terrain_mesher_tests|obj_export_golden_tests|solid_source_evaluation_tests|shader_source_tests)$'
    exit $LASTEXITCODE
} finally { Pop-Location }
