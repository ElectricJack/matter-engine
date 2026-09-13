param([string]$OutputDir = 'C:\tmp\matter-kreuzenstein-final')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ($OutputDir -match '\s') { throw 'The editor shot command requires an output path without spaces.' }
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$shots = $OutputDir.Replace('\','/')
$timeline = @"
wait_idle 2 90
render_path native_rt
set viewer.budget.pixel_budget 4
set render.lighting.sun_azimuth_deg -145
set render.lighting.sun_elevation_deg 42
set render.lighting.sun_tint 1,0.94,0.84
set render.lighting.sky_tint 1,0.9,0.7
set render.lighting.day_ambient_multiplier 0.2
set render.lighting.sky_irradiance_multiplier 0.3
set render.lighting.sun_multiplier 1.5
set render.lighting.sky_multiplier 0.65
set render.lighting.exposure_ev 0.1
cam 43 11 76 -1 21 2
wait_frames 120
stats castle-reference
shot $shots/reference.png
cam 23 14 34 12 15 9
wait_frames 90
shot $shots/entrance.png
cam 4 29 28 -7 30 0
wait_frames 90
shot $shots/chapel.png
cam 40 32 32 25 25 10
wait_frames 90
shot $shots/gallery.png
cam 28 6 35 8 0 22
wait_frames 90
shot $shots/bridge.png
cam 45 67 75 -1 18 -1
wait_frames 90
shot $shots/aerial.png
cam -54 40 -69 -4 23 -4
wait_frames 90
shot $shots/rear.png
quit
"@
$timelineFile = Join-Path $OutputDir 'capture.timeline'
[IO.File]::WriteAllText($timelineFile, $timeline + "`n", (New-Object Text.UTF8Encoding $false))
& py -3 (Join-Path $root 'MatterEngine3\tools\drive.py') `
    --world Kreuzenstein --timeline $timelineFile --out-dir (Join-Path $OutputDir 'run') `
    --editor (Join-Path $root 'MatterEditor\build\windows-msvc\editor.exe') `
    --timeout 240 --hide-ui --env MATTER_IMPOSTOR=0 `
    --env MATTER_WINDOW_WIDTH=1400 --env MATTER_WINDOW_HEIGHT=1000
if ($LASTEXITCODE -ne 0) { throw "Editor capture failed: $LASTEXITCODE" }
$log = Get-Content (Join-Path $OutputDir 'run\log.txt') -Raw
if ($log -match 'bake error|bake finished \([1-9]|flatten failed|validation errors: [1-9]') {
    throw 'The editor reported a bake, flatten, or validation failure.'
}
$archive = Join-Path $root 'build\qa\kreuzenstein\final'
New-Item -ItemType Directory -Force $archive | Out-Null
Copy-Item (Join-Path $OutputDir '*.png') $archive -Force
Copy-Item (Join-Path $OutputDir '*.done') $archive -Force
Copy-Item $timelineFile $archive -Force
Copy-Item (Join-Path $OutputDir 'run\log.txt') (Join-Path $archive 'editor.log') -Force
Write-Output "Verified castle screenshots: $archive"
