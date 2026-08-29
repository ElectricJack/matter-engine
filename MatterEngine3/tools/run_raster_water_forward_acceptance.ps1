[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BaselineDir,

    [Parameter(Mandatory = $true)]
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Command,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

function Resolve-OutputPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$editor = (Resolve-Path (Join-Path $repoRoot 'MatterEditor/build/windows-msvc/editor.exe')).Path
$drive = (Resolve-Path (Join-Path $repoRoot 'MatterEngine3/tools/drive.py')).Path
$perfTimeline = (Resolve-Path (Join-Path $repoRoot 'MatterEngine3/tools/raster_water_forward_perf.timeline')).Path
$captureTemplate = (Resolve-Path (Join-Path $repoRoot 'MatterEngine3/tools/raster_water_forward_acceptance.timeline')).Path
$comparator = (Resolve-Path (Join-Path $repoRoot 'MatterEngine3/tools/raster_water_forward_acceptance.py')).Path
$baseline = (Resolve-Path $BaselineDir).Path
$candidate = Resolve-OutputPath $OutputDir
Invoke-Checked -Description 'Raster-water acceptance path separation' -Command {
    & py -3 $comparator validate-paths `
        --baseline $baseline `
        --candidate $candidate
}
$screenshots = Join-Path $candidate 'screenshots'
$captureRun = Join-Path $candidate 'capture'
$expandedTimeline = Join-Path $candidate 'raster-water-forward-acceptance.timeline'

New-Item -ItemType Directory -Force $candidate, $screenshots, $captureRun | Out-Null
$screenshotToken = $screenshots.Replace('\', '/')
$timeline = (Get-Content -Raw $captureTemplate).Replace('{{OUTPUT_DIR}}', $screenshotToken)
[System.IO.File]::WriteAllText($expandedTimeline, $timeline, [System.Text.UTF8Encoding]::new($false))

Push-Location $repoRoot
try {
    Invoke-Checked -Description 'RiverFloatLab screenshot capture' -Command {
        & py -3 $drive --world RiverFloatLab `
            --timeline $expandedTimeline `
            --out-dir $captureRun `
            --timeout 4200 `
            --editor $editor `
            --env MATTER_WINDOW_WIDTH=1280 `
            --env MATTER_WINDOW_HEIGHT=720 `
            --env MATTER_SUN_SHADOW_SAMPLES=16
    }

    # The engine's .done files are intentionally empty sentinels. Annotate the
    # acceptance copies after drive.py has verified this run produced them so
    # the independently portable evidence set can reject zero-byte artifacts.
    foreach ($name in @(
        'shallow-player-low.png',
        'upper-rapids.png',
        'waterfall-side.png',
        'plunge-pool.png',
        'section-handoff.png')) {
        $sidecar = Join-Path $screenshots "$name.done"
        if (-not (Test-Path -LiteralPath $sidecar -PathType Leaf)) {
            throw "Screenshot sidecar was not produced: $sidecar"
        }
        [System.IO.File]::WriteAllText(
            $sidecar, "captured by raster-water forward acceptance`n",
            [System.Text.UTF8Encoding]::new($false))
    }

    foreach ($samples in 1, 10, 16) {
        $tag = '{0:D2}' -f $samples
        $runDir = Join-Path $candidate "run-$tag"
        $perfOutput = Join-Path $candidate "shadow-$tag.json"
        New-Item -ItemType Directory -Force $runDir | Out-Null
        # A prior interrupted run must never satisfy the comparator. Remove
        # this exact evidence file before launch and require the editor to
        # recreate a nonempty file during the checked run.
        Remove-Item -LiteralPath $perfOutput -Force -ErrorAction SilentlyContinue
        Invoke-Checked -Description "RiverFloatLab shadow-$tag performance run" -Command {
            & py -3 $drive --world RiverFloatLab `
                --timeline $perfTimeline `
                --out-dir $runDir `
                --timeout 4200 `
                --editor $editor `
                --hide-ui `
                --env MATTER_WINDOW_WIDTH=1280 `
                --env MATTER_WINDOW_HEIGHT=720 `
                --env "MATTER_SUN_SHADOW_SAMPLES=$samples" `
                --env "MATTER_PERF_OUTPUT=$perfOutput" `
                --env MATTER_PERF_WARMUP_SECONDS=20 `
                --env MATTER_PERF_SAMPLE_SECONDS=30
        }
        if (-not (Test-Path -LiteralPath $perfOutput -PathType Leaf) -or
            (Get-Item -LiteralPath $perfOutput).Length -eq 0) {
            throw "RiverFloatLab shadow-$tag did not produce fresh performance JSON: $perfOutput"
        }
    }

    Invoke-Checked -Description 'Raster-water forward comparator' -Command {
        & py -3 $comparator compare `
            --baseline $baseline `
            --candidate $candidate `
            --screenshots $screenshots
    }
}
finally {
    Pop-Location
}
