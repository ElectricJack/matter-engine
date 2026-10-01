[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepositoryRoot,
    [ValidateSet('build', 'test', 'smoke')][string]$Phase = 'test'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath $RepositoryRoot).Path
Import-Module (Join-Path $repo 'tools/windows/MatterWindowsToolchain.psm1') -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repo
$bin = Split-Path $toolchain.CMake
$build = Join-Path $repo 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo'
$targets = @('world_definition_tests', 'eval_world_tests', 'terrain_field_tests',
    'terrain_mesher_tests', 'surface_field_tests', 'sector_lod_tests',
    'sector_bake_tests', 'vt_residency_tests', 'vertex_cache_order_tests',
    'world_tracer_tests', 'gi_bake_tests', 'obj_export_tests',
    'obj_export_golden_tests', 'shader_source_tests')
if ($Phase -eq 'build') {
    $developer = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
    $command = '{0} && "{1}" --build "{2}" --target {3}' -f $developer, $toolchain.CMake, $build, ($targets -join ' ')
    & $env:ComSpec /d /s /c $command
    exit $LASTEXITCODE
}
if ($Phase -eq 'test') {
    $names = $targets + @('viewer_graph_tests', 'geometry_runtime_tests',
        'geometry_runtime_gpu_tests', 'partstore_tests', 'geometry_hierarchy_tests',
        'geometry_cut_tests', 'geometry_pages_gpu_tests', 'vt_surface_material_tests',
        'smoke_vt_normal_frame')
    & (Join-Path $bin 'ctest.exe') --test-dir $build -j 1 --output-on-failure -R ('^(' + ($names -join '|') + ')$')
    exit $LASTEXITCODE
}
$failed = @()
Push-Location $build
try {
    foreach ($mode in @('rt', 'rt-transmission', 'rt-local-direct', 'vt-composed-parallax')) {
        $env:MATTER_VK_SMOKE_MODE = $mode
        Write-Output "MODE=$mode"
        & (Join-Path $build 'vulkan_smoke_tests.exe')
        if ($LASTEXITCODE -ne 0) { $failed += $mode }
    }
} finally {
    Remove-Item Env:MATTER_VK_SMOKE_MODE -ErrorAction SilentlyContinue
    Pop-Location
}
if ($failed.Count) { throw ('Failed smoke modes: ' + ($failed -join ', ')) }
