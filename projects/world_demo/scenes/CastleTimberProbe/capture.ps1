param([string]$OutputDir = 'D:\tmp\matter-castle-timber-probe')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ($OutputDir -match '\s') { throw 'Editor capture paths must not contain spaces.' }
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$shots = $OutputDir.Replace('\','/')
$timeline = @"
wait_idle 2 300
set viewer.budget.pixel_budget 4
render_path raster
cam 2.6 2.0 2.7 0.1 0.64 0.04
wait_frames 90
stats timber-raster
shot $shots/raster-overview.png
cam 0.22 1.03 1.02 0.0 0.78 0.45
wait_frames 90
shot $shots/raster-plank-grooves.png
render_path native_rt
cam 2.6 2.0 2.7 0.1 0.64 0.04
wait_frames 180
stats timber-native-rt
shot $shots/rt-overview.png
cam 0.22 1.03 1.02 0.0 0.78 0.45
wait_frames 120
shot $shots/rt-plank-grooves.png
cam 1.28 1.04 0.32 0.28 0.76 -0.30
wait_frames 120
shot $shots/rt-beam-joinery.png
cam 1.86 0.72 0.76 1.2 0.36 0.05
wait_frames 120
shot $shots/rt-upright-leg.png
quit
"@
$timelineFile = Join-Path $OutputDir 'capture.timeline'
[IO.File]::WriteAllText($timelineFile, $timeline + "`n", (New-Object Text.UTF8Encoding $false))
& py -3 (Join-Path $root 'MatterEngine3\tools\drive.py') `
    --world CastleTimberProbe --timeline $timelineFile --out-dir (Join-Path $OutputDir 'run') `
    --editor (Join-Path $root 'MatterEditor\build\windows-msvc\editor.exe') `
    --timeout 600 --hide-ui --env MATTER_IMPOSTOR=0 `
    --env MATTER_WINDOW_WIDTH=1600 --env MATTER_WINDOW_HEIGHT=1100
if ($LASTEXITCODE -ne 0) { throw "Editor capture failed: $LASTEXITCODE" }
$logFile = Join-Path $OutputDir 'run\log.txt'
$log = Get-Content $logFile -Raw
if ($log -match 'bake error|bake finished \([1-9]|flatten failed|validation errors: [1-9]|invalid vertex coordinates') {
    throw 'The timber fixture reported a bake, geometry, flatten, or validation failure.'
}
$archive = Join-Path $root 'build\qa\castle-timber-probe'
New-Item -ItemType Directory -Force $archive | Out-Null
Copy-Item (Join-Path $OutputDir '*.png') $archive -Force
Copy-Item $timelineFile $archive -Force
Copy-Item $logFile (Join-Path $archive 'editor.log') -Force
Write-Output "Timber captures ready for visual review: $archive"
