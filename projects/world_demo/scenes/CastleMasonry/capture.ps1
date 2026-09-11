param([string]$OutputDir = 'C:\tmp\matter-castle-masonry')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ($OutputDir -match '\s') { throw 'The editor shot command requires an output path without spaces.' }
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$shots = $OutputDir.Replace('\','/')

# Camera coordinates below are computed from the compiled
# CASTLE_MASONRY_FIXTURE_PLAN manifest (see README.md's camera table for the
# derivation of each), not guessed. Each shot is taken once per render path
# so a regression on only one path cannot slip through.
function ShotsForPass([string]$prefix) {
    return @"
cam 17 16 -13 1 2 1
wait_frames 90
shot $shots/${prefix}_overview.png
cam 13 4 9 7 2 5
wait_frames 90
shot $shots/${prefix}_apse-join.png
cam 4 1.7 3 -3 1.7 3
wait_frames 90
shot $shots/${prefix}_tower-throat.png
cam 0.5 1.8 -6 2 1.5 -4
wait_frames 90
shot $shots/${prefix}_window-reveal.png
cam 8 5 -7 4 1.8 -2
wait_frames 90
shot $shots/${prefix}_cross-junction.png
cam 6 1.6 -3.3 6 1.6 -1
wait_frames 90
shot $shots/${prefix}_interior-arch.png
"@
}

# A cold cache bakes ~100 voxel stone variants: wait for bake.finished
# BEFORE wait_idle (wait_idle only releases once the bake is done).
$timeline = @"
wait_event bake.finished 900
wait_idle 2 120
set viewer.budget.pixel_budget 2
render_path raster
wait_frames 30
$(ShotsForPass 'raster')
render_path native_rt
wait_frames 30
$(ShotsForPass 'native_rt')
quit
"@
$timelineFile = Join-Path $OutputDir 'capture.timeline'
[IO.File]::WriteAllText($timelineFile, $timeline + "`n", (New-Object Text.UTF8Encoding $false))
& py -3 (Join-Path $root 'MatterEngine3\tools\drive.py') `
    --world CastleMasonry --timeline $timelineFile --out-dir (Join-Path $OutputDir 'run') `
    --editor (Join-Path $root 'MatterEditor\build\windows-msvc\editor.exe') `
    --timeout 1500 --hide-ui --env MATTER_IMPOSTOR=0 `
    --env MATTER_WINDOW_WIDTH=1400 --env MATTER_WINDOW_HEIGHT=1000
if ($LASTEXITCODE -ne 0) { throw "Editor capture failed: $LASTEXITCODE" }
$log = Get-Content (Join-Path $OutputDir 'run\log.txt') -Raw
if ($log -match 'bake error|bake finished \([1-9]|flatten failed|validation errors: [1-9]') {
    throw 'The editor reported a bake, flatten, or validation failure.'
}
$archive = Join-Path $root 'build\qa\castle-masonry'
New-Item -ItemType Directory -Force $archive | Out-Null
Copy-Item (Join-Path $OutputDir '*.png') $archive -Force
Copy-Item (Join-Path $OutputDir '*.done') $archive -Force
Copy-Item $timelineFile $archive -Force
Copy-Item (Join-Path $OutputDir 'run\log.txt') (Join-Path $archive 'editor.log') -Force
Write-Output "Verified castle masonry screenshots: $archive"
