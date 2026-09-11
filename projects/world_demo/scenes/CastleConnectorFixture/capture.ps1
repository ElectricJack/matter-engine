param([string]$OutputDir = 'D:\tmp\matter-castle-connector-fixture')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ($OutputDir -match '\s') { throw 'Editor capture paths must not contain spaces.' }
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$shots = $OutputDir.Replace('\','/')
$timeline = @"
wait_idle 2 300
set viewer.budget.pixel_budget 3
render_path raster
cam 18 14 29 7 1.5 3
wait_frames 120
stats connectors-raster
shot $shots/connectors-overview.png
cam 6.3 2.2 7.8 4.0 1.4 8.5
wait_frames 90
shot $shots/connector-30-joints.png
cam 20.0 2.1 -1.2 16.8 1.2 -4.0
wait_frames 90
shot $shots/connector-45-interior.png
quit
"@
$timelineFile = Join-Path $OutputDir 'capture.timeline'
[IO.File]::WriteAllText($timelineFile, $timeline + "`n", (New-Object Text.UTF8Encoding $false))
& py -3 (Join-Path $root 'MatterEngine3\tools\drive.py') `
    --world CastleConnectorFixture --timeline $timelineFile --out-dir (Join-Path $OutputDir 'run') `
    --editor (Join-Path $root 'MatterEditor\build\windows-msvc\editor.exe') `
    --timeout 600 --hide-ui --env MATTER_IMPOSTOR=0 `
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
Write-Output "Connector captures ready for visual review: $archive"
