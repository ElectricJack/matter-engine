param(
    [string]$ViewerPath = "$PSScriptRoot\..\viewer.exe",
    [string]$World = 'StressForest50k',
    [double]$WarmupSeconds = 10,
    [double]$SampleSeconds = 20,
    [double]$MinimumFps = 55
)

$ErrorActionPreference = 'Stop'
$result = Join-Path $env:TEMP 'matter-vulkan-instancing-perf.json'
Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
$env:MATTER_WORLD = $World
$env:MATTER_CAM = '0,18,45,0,8,0'
$env:MATTER_PERF_OUTPUT = $result
$env:MATTER_PERF_WARMUP_SECONDS = [string]$WarmupSeconds
$env:MATTER_PERF_SAMPLE_SECONDS = [string]$SampleSeconds

& $ViewerPath
if ($LASTEXITCODE -ne 0) { throw "viewer exited $LASTEXITCODE" }

$sample = Get-Content -Raw $result | ConvertFrom-Json
if ($sample.median_fps -lt $MinimumFps) {
    throw "median FPS $($sample.median_fps) is below $MinimumFps"
}
if ($sample.static_vertex_upload_delta -ne 0 -or
    $sample.static_cluster_upload_delta -ne 0 -or
    $sample.stable_instance_upload_delta -ne 0 -or
    $sample.immediate_submit_delta -ne 0) {
    throw 'stable interval performed forbidden uploads or immediate submits'
}
