param([string]$OutputDir = 'D:\tmp\matter-castle-connector-fixture')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ($OutputDir -match '\s') { throw 'Editor capture paths must not contain spaces.' }
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$shots = $OutputDir.Replace('\','/')
$timeline = @"
set viewer.budget.pixel_budget 3
set render.lighting.exposure_ev 0
render_path raster
cam 18 14 29 7 1.5 3
wait_frames 120
stats connectors-raster
shot $shots/connectors-overview.png
cam -0.65 1.65 9 4.5 1.9 9
wait_frames 90
shot $shots/connector-30-joints.png
cam 13.35 1.65 -3 18.5 1.9 -3
wait_frames 90
shot $shots/connector-45-interior.png
cam -0.65 1.65 -3 4.5 1.9 -3
wait_frames 90
shot $shots/connector-15-interior.png
cam 13.35 1.65 9 18.5 1.9 9
wait_frames 90
shot $shots/connector-neg30-interior.png
render_path native_rt
cam 3 1.65 9 5 3.65 9
wait_frames 180
shot $shots/connector-30-underside-rt.png
quit
"@
$timelineFile = Join-Path $OutputDir 'capture.timeline'
[IO.File]::WriteAllText($timelineFile, $timeline + "`n", (New-Object Text.UTF8Encoding $false))
& py -3 (Join-Path $root 'tools\castle_scene_capture.py') `
    --world CastleConnectorFixture --timeline $timelineFile --out-dir (Join-Path $OutputDir 'run') `
    --editor (Join-Path $root 'MatterEditor\build\windows-msvc\editor.exe') `
    --editor-dir (Join-Path $root 'MatterEditor') `
    --timeout 1200 --env MATTER_IMPOSTOR=0 `
    --env MATTER_WINDOW_WIDTH=1600 --env MATTER_WINDOW_HEIGHT=1100
if ($LASTEXITCODE -ne 0) { throw "Editor capture failed: $LASTEXITCODE" }
$logFile = Join-Path $OutputDir 'run\log.txt'
$log = Get-Content $logFile -Raw
if ($log -match 'bake error|bake finished \([1-9]|flatten failed|validation errors: [1-9]|invalid vertex coordinates|HullBuildFailed') {
    throw 'Connector fixture reported a bake, geometry, collision, flatten, or validation failure.'
}
$archive = Join-Path $root 'build\qa\castle-connectors'
New-Item -ItemType Directory -Force $archive | Out-Null
Copy-Item (Join-Path $OutputDir '*.png') $archive -Force
Copy-Item $timelineFile $archive -Force
Copy-Item $logFile (Join-Path $archive 'editor.log') -Force
Copy-Item (Join-Path $OutputDir 'run\capture.json') $archive -Force
Write-Output "Connector captures ready for visual review: $archive"
