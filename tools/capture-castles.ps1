param(
    [ValidateSet('CastleCourtyard', 'CastleRoundKeep', 'CastleCloister', 'CastleGallery')]
    [string]$World = 'CastleGallery',
    [string]$OutputDir = 'D:\tmp\matter-castles',
    [int]$TimeoutSeconds = 900
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$OutputDir = [IO.Path]::GetFullPath((Join-Path $OutputDir $World))
if ($OutputDir -match '\s') { throw 'The editor shot command requires an output path without spaces.' }
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$shots = $OutputDir.Replace('\', '/')
$views = switch ($World) {
    'CastleCourtyard' { @(
        @('exterior', '58 40 72 18 6 20'),
        @('entrance', '18 2.0 44 18 2.1 30'),
        @('hall', '12 1.7 5 25 2.4 9'),
        @('balcony', '18 5.7 11 18 2.2 5'),
        @('court', '16 1.7 23 26 5.0 15'),
        @('chapel', '31 1.7 26 31 4.0 14')
    ) }
    'CastleRoundKeep' { @(
        @('exterior', '45 32 48 9 6 14'),
        @('entrance', '12 1.7 31 12 2.2 23'),
        @('hall', '9 1.7 18 16 4.0 10'),
        @('balcony', '5 5.7 15 17 2.5 15'),
        @('tower-door', '6 1.7 4 0 1.8 4'),
        @('stair', '2.3 1.7 2 0 2.0 5.4')
    ) }
    'CastleCloister' { @(
        @('exterior', '70 38 59 22 6 14'),
        @('entrance', '16 1.7 31 16 2.0 22'),
        @('hall', '13 1.7 4 27 2.2 4'),
        @('cloister', '9 1.7 16 9 2.0 9'),
        @('chapel', '39 1.7 17 39 4.2 10'),
        @('choir', '41 5.7 19 39 2.8 10'),
        @('landing', '39 5.7 20.8 40 5.4 24')
    ) }
    'CastleGallery' { @(
        @('gallery', '90 85 145 10 8 16'),
        @('courtyard', '3 40 72 -37 6 20'),
        @('roundkeep', '45 32 48 9 6 14'),
        @('cloister', '110 38 59 62 6 14')
    ) }
}
$lines = [Collections.Generic.List[string]]::new()
@(
    'wait_idle 2 600',
    'render_path native_rt',
    'set viewer.budget.pixel_budget 4',
    'set render.lighting.sun_azimuth_deg -140',
    'set render.lighting.sun_elevation_deg 38',
    'set render.lighting.sun_tint 1,0.94,0.84',
    'set render.lighting.sky_tint 1,0.96,0.9',
    'set render.lighting.day_ambient_multiplier 0.2',
    'set render.lighting.sky_irradiance_multiplier 0.45',
    'set render.lighting.sun_multiplier 1.4',
    'set render.lighting.sky_multiplier 0.7',
    'set render.lighting.exposure_ev 0.3'
) | ForEach-Object { $lines.Add($_) }
foreach ($view in $views) {
    $lines.Add("cam $($view[1])")
    $lines.Add('wait_frames 120')
    $lines.Add("stats $($view[0])")
    $lines.Add("shot $shots/$($view[0]).png")
}
$lines.Add('set render.lighting.sun_elevation_deg -5')
$lines.Add('set render.lighting.exposure_ev 0.8')
$lines.Add("cam $($views[0][1])")
$lines.Add('wait_frames 180')
$lines.Add("shot $shots/dusk.png")
$lines.Add('quit')
$timelineFile = Join-Path $OutputDir 'capture.timeline'
[IO.File]::WriteAllText($timelineFile, ($lines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
& py -3 (Join-Path $root 'MatterEngine3\tools\drive.py') `
    --world $World --timeline $timelineFile --out-dir (Join-Path $OutputDir 'run') `
    --editor (Join-Path $root 'MatterEditor\build\windows-msvc\editor.exe') `
    --timeout $TimeoutSeconds --hide-ui --env MATTER_IMPOSTOR=0 `
    --env MATTER_WINDOW_WIDTH=1920 --env MATTER_WINDOW_HEIGHT=1080
if ($LASTEXITCODE -ne 0) { throw "Editor capture failed: $LASTEXITCODE" }
$log = Get-Content (Join-Path $OutputDir 'run\log.txt') -Raw
if ($log -match 'bake error|bake finished \([1-9]|flatten failed|validation errors: [1-9]') {
    throw 'The editor reported a bake, flatten, or validation failure.'
}
Write-Output "Verified castle screenshots: $OutputDir"
