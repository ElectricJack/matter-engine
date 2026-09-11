param([string]$OutputDir = 'C:\tmp\local-light-rt-reopen')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ($OutputDir -match '\s') { throw 'The editor shot command requires an output path without spaces.' }
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$shots = $OutputDir.Replace('\','/')

# Each A/B pair has one camera command and no later camera mutation.  Both
# sides explicitly restore every live lighting control except the single
# variable under test, reset history, and accumulate the same frame count.
$timeline = @"
wait_event bake.finished 900
wait_idle 2 120
render_path native_rt
set render.gi.diffuse_multiplier 1
set render.lighting.emission_multiplier 1
set render.lighting.exposure_ev 0

cam 5.8 3.2 7.4 1.5 1.30 1.05
set render.gi.enabled false
history_reset
wait_frames 96
shot $shots/corner-gi-off.png
set render.gi.enabled true
history_reset
wait_frames 96
shot $shots/corner-gi-on.png

cam 14.0 2.85 6.2 13.5 1.45 0.1
set render.gi.enabled false
history_reset
wait_frames 96
shot $shots/materials-gi-off.png
set render.gi.enabled true
history_reset
wait_frames 96
shot $shots/materials-gi-on.png

cam 12.8 3.1 5.5 11.5 1.8 1.0
set render.gi.enabled true
set render.lighting.emission_multiplier 1
history_reset
wait_frames 96
shot $shots/proxy-visible.png
set render.lighting.emission_multiplier 0
history_reset
wait_frames 96
shot $shots/proxy-hidden.png
set render.lighting.emission_multiplier 1
quit
"@

$timelineFile = Join-Path $OutputDir 'capture.timeline'
[IO.File]::WriteAllText($timelineFile, $timeline + "`n", (New-Object Text.UTF8Encoding $false))
& py -3 (Join-Path $root 'MatterEngine3\tools\drive.py') `
    --world LocalLightRtGallery --timeline $timelineFile `
    --out-dir (Join-Path $OutputDir 'run') `
    --editor (Join-Path $root 'MatterEditor\build\windows-msvc\editor.exe') `
    --timeout 1500 --hide-ui `
    --env MATTER_WINDOW_WIDTH=1400 --env MATTER_WINDOW_HEIGHT=1000
if ($LASTEXITCODE -ne 0) { throw "Editor capture failed: $LASTEXITCODE" }

$log = Get-Content (Join-Path $OutputDir 'run\log.txt') -Raw
if ($log -match 'native_rt unavailable|set: unknown property|set: .* is read-only|set: .* is forced by|set: cannot parse|event: bake\.finished timeout|idle: timeout|wait_frames: dispatch failed|bake error|bake finished \([1-9]|flatten failed|validation errors: [1-9]') {
    throw 'The editor rejected a capture control or reported a bake/validation failure.'
}
Write-Output "Verified reopened local-light screenshots: $OutputDir"
