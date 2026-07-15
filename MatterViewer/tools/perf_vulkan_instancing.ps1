param(
    [string]$ViewerPath = "$PSScriptRoot\..\viewer.exe",
    [string]$World = 'StressForest50k',
    [double]$WarmupSeconds = 10,
    [double]$SampleSeconds = 20,
    [double]$MinimumFps = 55
)

$ErrorActionPreference = 'Stop'
$savedRuntimeEnvironment = @{
    VK_LAYER_PATH = [Environment]::GetEnvironmentVariable('VK_LAYER_PATH', 'Process')
    PATH = [Environment]::GetEnvironmentVariable('PATH', 'Process')
}
$savedMatterEnvironment = @{}
@((Get-ChildItem Env:) | Where-Object { $_.Name -like 'MATTER_*' }) |
    ForEach-Object {
        $savedMatterEnvironment[$_.Name] = $_.Value
        Remove-Item -LiteralPath ("Env:" + $_.Name)
    }
$runRoot = Join-Path $env:TEMP ("matter-vulkan-instancing-perf-" +
                                [guid]::NewGuid().ToString('N'))
$result = Join-Path $runRoot 'result.json'

try {
    $layerCandidates = @()
    if ($env:VULKAN_SDK) { $layerCandidates += (Join-Path $env:VULKAN_SDK 'Bin') }
    $layerCandidates += 'C:\msys64\ucrt64\bin'
    $layerCandidates += 'C:\msys64\ucrt64\share\vulkan\explicit_layer.d'
    $layerDir = $layerCandidates | Where-Object {
        Test-Path (Join-Path $_ 'VkLayer_khronos_validation.json')
    } | Select-Object -First 1
    if (-not $layerDir) {
        throw 'Vulkan validation layer unavailable; install MSYS2 vulkan-validation-layers'
    }
    $env:VK_LAYER_PATH = $layerDir
    $msysBin = 'C:\msys64\ucrt64\bin'
    if (Test-Path $msysBin) { $env:PATH = "$msysBin;$env:PATH" }
    New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
    $env:MATTER_WORLD = $World
    $env:MATTER_CAM = '0,18,45,0,8,0'
    $env:MATTER_CACHE_ROOT = Join-Path $runRoot 'cache'
    $env:MATTER_PERF_OUTPUT = $result
    $env:MATTER_PERF_WARMUP_SECONDS = [string]$WarmupSeconds
    $env:MATTER_PERF_SAMPLE_SECONDS = [string]$SampleSeconds

    & $ViewerPath
    if ($LASTEXITCODE -ne 0) { throw "viewer exited $LASTEXITCODE" }

    $sample = Get-Content -Raw $result | ConvertFrom-Json
    if ($sample.frame_metric -ne 'end_to_end_cadence') {
        throw "unexpected frame metric '$($sample.frame_metric)'"
    }
    if ($sample.median_fps -lt $MinimumFps) {
        throw "median FPS $($sample.median_fps) is below $MinimumFps"
    }
    if ($sample.static_vertex_upload_delta -ne 0 -or
        $sample.static_cluster_upload_delta -ne 0 -or
        $sample.stable_instance_upload_delta -ne 0 -or
        $sample.immediate_submit_delta -ne 0) {
        throw 'stable interval performed forbidden uploads or immediate submits'
    }
    foreach ($field in @('selected_dlss_mode','active_dlss_mode',
                          'dlss_internal_width','dlss_internal_height',
                          'dlss_output_width','dlss_output_height',
                          'dlss_reset_delta','rt_available','rt_enabled',
                          'rt_samples','rt_debug_view','fallback_reason')) {
        if ($null -eq $sample.PSObject.Properties[$field]) {
            throw "performance JSON missing RTX/DLSS field '$field'"
        }
    }
    $validModes = @('Native','Quality','Balanced','Performance')
    if ($sample.selected_dlss_mode -notin $validModes -or
        $sample.active_dlss_mode -notin $validModes) {
        throw "invalid DLSS mode selected='$($sample.selected_dlss_mode)' active='$($sample.active_dlss_mode)'"
    }
    if ($sample.dlss_internal_width -le 0 -or
        $sample.dlss_internal_height -le 0 -or
        $sample.dlss_output_width -le 0 -or
        $sample.dlss_output_height -le 0) {
        throw 'DLSS performance extents must be positive'
    }
    if ($sample.active_dlss_mode -eq 'Native' -and
        ($sample.dlss_internal_width -ne $sample.dlss_output_width -or
         $sample.dlss_internal_height -ne $sample.dlss_output_height)) {
        throw 'Native mode was mislabeled with a non-native internal extent'
    }
    if ($sample.selected_dlss_mode -ne $sample.active_dlss_mode -and
        [string]::IsNullOrWhiteSpace([string]$sample.fallback_reason)) {
        throw 'DLSS fallback was mislabeled without a fallback reason'
    }
    if ($sample.active_dlss_mode -ne 'Native' -and
        -not [string]::IsNullOrWhiteSpace([string]$sample.fallback_reason)) {
        throw 'active DLSS was mislabeled as a fallback'
    }
    if ($sample.dlss_reset_delta -ne 0) {
        throw "DLSS history reset persisted through stable sampling ($($sample.dlss_reset_delta) resets)"
    }
    if ($sample.rt_enabled -and -not $sample.rt_available) {
        throw 'ray tracing was reported enabled while unavailable'
    }
    if ($sample.rt_samples -lt 1) {
        throw "invalid ray tracing sample count $($sample.rt_samples)"
    }
    if ($sample.validation_errors -ne 0) {
        throw "viewer reported $($sample.validation_errors) Vulkan validation errors"
    }
    Write-Output (("Vulkan instancing performance: {0:N2} FPS, median {1:N2} ms, " +
                   "p95 {2:N2} ms, DLSS {3}/{4} {5}x{6}->{7}x{8}, " +
                   "RT available={9} enabled={10} samples={11}, fallback='{12}'") -f
                   $sample.median_fps, $sample.median_frame_ms,
                   $sample.p95_frame_ms, $sample.selected_dlss_mode,
                   $sample.active_dlss_mode, $sample.dlss_internal_width,
                   $sample.dlss_internal_height, $sample.dlss_output_width,
                   $sample.dlss_output_height, $sample.rt_available,
                   $sample.rt_enabled, $sample.rt_samples,
                   $sample.fallback_reason)
} finally {
    @((Get-ChildItem Env:) | Where-Object { $_.Name -like 'MATTER_*' }) |
        ForEach-Object { Remove-Item -LiteralPath ("Env:" + $_.Name) }
    foreach ($entry in $savedMatterEnvironment.GetEnumerator()) {
        Set-Item -LiteralPath ("Env:" + $entry.Key) -Value $entry.Value
    }
    foreach ($entry in $savedRuntimeEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
    Remove-Item -LiteralPath $runRoot -Recurse -Force -ErrorAction SilentlyContinue
}
