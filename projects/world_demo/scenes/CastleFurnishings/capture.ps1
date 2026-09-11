param([string]$OutputDir = 'C:\tmp\matter-castle-furnishings')
# Native fixture capture: raster + native RT overview and close-ups of every
# furnishing family, the candle fixtures and the three glazing variants.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ($OutputDir -match '\s') { throw 'The editor shot command requires an output path without spaces.' }
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$shots = $OutputDir.Replace('\','/')
$timeline = @"
wait_idle 2 180
render_path raster
cam 5.2 3.0 5.4 -1.2 1.1 -1.4
wait_frames 90
stats furnishings-raster
shot $shots/raster-overview.png
cam -2.1 2.35 -3.2 -2.6 2.45 -3.8
wait_frames 60
shot $shots/raster-sconce.png
render_path native_rt
set viewer.budget.pixel_budget 4
cam 5.2 3.0 5.4 -1.2 1.1 -1.4
wait_frames 150
stats furnishings-rt
shot $shots/rt-overview.png
cam 1.6 1.9 2.8 -1.8 0.9 0.0
wait_frames 90
shot $shots/rt-hall.png
cam -1.6 1.3 1.4 -3.0 0.9 0.3
wait_frames 90
shot $shots/rt-throne.png
cam -2.2 1.9 0.8 -4.6 0.7 -2.4
wait_frames 90
shot $shots/rt-chamber.png
cam -3.0 1.2 1.9 -5.2 0.7 0.6
wait_frames 90
shot $shots/rt-storage.png
cam -3.9 1.1 3.9 -5.0 0.4 3.0
wait_frames 90
shot $shots/rt-barrels.png
cam 2.2 1.6 -0.8 3.6 1.8 -4.0
wait_frames 90
shot $shots/rt-chapel.png
cam 3.1 2.9 -2.2 3.6 3.2 -4.2
wait_frames 90
shot $shots/rt-stained-window.png
cam -1.2 2.6 -2.2 -1.2 3.0 -4.2
wait_frames 90
shot $shots/rt-tracery.png
cam 4.6 2.0 1.0 6.0 2.0 1.2
wait_frames 90
shot $shots/rt-thin-window.png
cam -2.1 2.35 -3.2 -2.6 2.45 -3.8
wait_frames 90
shot $shots/rt-sconce.png
cam -5.0 2.5 -0.1 -5.55 2.45 -0.6
wait_frames 90
shot $shots/rt-lantern.png
cam 0.3 3.1 1.6 -1.2 3.0 0.3
wait_frames 90
shot $shots/rt-chandelier.png
render_path raster
set render.lighting.sun_multiplier 0
set render.lighting.sky_multiplier 0.04
set render.lighting.day_ambient_multiplier 0
cam 5.2 3.0 5.4 -1.2 1.1 -1.4
wait_frames 120
stats furnishings-night-raster
shot $shots/night-raster-overview.png
cam 1.6 1.9 2.8 -1.8 0.9 0.0
wait_frames 60
shot $shots/night-raster-hall.png
cam -1.2 2.0 -1.0 -1.2 2.2 -4.0
wait_frames 60
shot $shots/night-raster-sconces.png
cam -2.5 2.2 1.5 -4.8 0.8 -0.6
wait_frames 60
shot $shots/night-raster-lantern-spot.png
cam 2.2 1.6 -0.8 3.6 1.2 -3.4
wait_frames 60
shot $shots/night-raster-altar.png
quit
"@
$timelineFile = Join-Path $OutputDir 'capture.timeline'
[IO.File]::WriteAllText($timelineFile, $timeline + "`n", (New-Object Text.UTF8Encoding $false))
& py -3 (Join-Path $root 'MatterEngine3\tools\drive.py') `
    --world CastleFurnishings --timeline $timelineFile --out-dir (Join-Path $OutputDir 'run') `
    --editor (Join-Path $root 'MatterEditor\build\windows-msvc\editor.exe') `
    --timeout 600 --hide-ui --env MATTER_IMPOSTOR=0 `
    --env MATTER_WINDOW_WIDTH=1400 --env MATTER_WINDOW_HEIGHT=1000
if ($LASTEXITCODE -ne 0) { throw "Editor capture failed: $LASTEXITCODE" }
$log = Get-Content (Join-Path $OutputDir 'run\log.txt') -Raw
if ($log -match 'bake error|bake finished \([1-9]|flatten failed|validation errors: [1-9]') {
    throw 'The editor reported a bake, flatten, or validation failure.'
}
$archive = Join-Path $root 'build\qa\castle-furnishings'
New-Item -ItemType Directory -Force $archive | Out-Null
Copy-Item (Join-Path $OutputDir '*.png') $archive -Force
Copy-Item $timelineFile $archive -Force
Copy-Item (Join-Path $OutputDir 'run\log.txt') (Join-Path $archive 'editor.log') -Force
Write-Output "Verified furnishing screenshots: $archive"
