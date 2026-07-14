param(
    [string]$ViewerPath = "$PSScriptRoot\..\viewer.exe",
    [string]$World = 'StressForest50k',
    [double]$WarmupSeconds = 10,
    [double]$SampleSeconds = 20,
    [double]$MinimumFps = 55
)

$ErrorActionPreference = 'Stop'
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
    if ($sample.validation_errors -ne 0) {
        throw "viewer reported $($sample.validation_errors) Vulkan validation errors"
    }
    Write-Output (("Vulkan instancing performance: {0:N2} FPS, median {1:N2} ms, " +
                   "p95 {2:N2} ms") -f $sample.median_fps,
                   $sample.median_frame_ms, $sample.p95_frame_ms)
} finally {
    @((Get-ChildItem Env:) | Where-Object { $_.Name -like 'MATTER_*' }) |
        ForEach-Object { Remove-Item -LiteralPath ("Env:" + $_.Name) }
    foreach ($entry in $savedMatterEnvironment.GetEnumerator()) {
        Set-Item -LiteralPath ("Env:" + $entry.Key) -Value $entry.Value
    }
    Remove-Item -LiteralPath $runRoot -Recurse -Force -ErrorAction SilentlyContinue
}
