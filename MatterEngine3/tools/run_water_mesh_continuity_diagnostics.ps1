[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-OutputPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Command,
        [Parameter(Mandatory = $true)][string]$Description
    )
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$editor = (Resolve-Path (Join-Path $repoRoot 'MatterEditor/build/windows-msvc/editor.exe')).Path
$drive = (Resolve-Path (Join-Path $repoRoot 'MatterEngine3/tools/drive.py')).Path
$templatePath = (Resolve-Path (Join-Path $PSScriptRoot 'water_mesh_continuity_diagnostics.timeline')).Path
$outputRoot = Resolve-OutputPath $OutputDir
$diagnostics = Join-Path $outputRoot 'diagnostics'
$template = Get-Content -Raw $templatePath
$frames = 0, 7, 15, 22, 29
$views = @(
    @{ Name = 'normal'; Value = $null },
    @{ Name = 'identity'; Value = 'identity' },
    @{ Name = 'geometry-normal'; Value = 'geometry-normal' },
    @{ Name = 'foam-driver'; Value = 'foam-driver' }
)
$cameraNames = 'waterfall-side.png', 'section-handoff.png'

New-Item -ItemType Directory -Force $diagnostics | Out-Null
$hadPriorDiagnostic = Test-Path Env:MATTER_WATER_DIAGNOSTIC_VIEW
$priorDiagnostic = if ($hadPriorDiagnostic) {
    $env:MATTER_WATER_DIAGNOSTIC_VIEW
} else {
    $null
}

Push-Location $repoRoot
try {
    foreach ($frame in $frames) {
        foreach ($view in $views) {
            $prefix = 'frame-{0:D2}-{1}' -f $frame, $view.Name
            $captureDir = Join-Path $diagnostics $prefix
            $runDir = Join-Path $outputRoot "run-$prefix"
            $timelinePath = Join-Path $runDir 'water-mesh-continuity.timeline'
            New-Item -ItemType Directory -Force $captureDir, $runDir | Out-Null
            $captureToken = $captureDir.Replace('\', '/')
            $expanded = $template.Replace('{{OUTPUT_DIR}}', $captureToken)
            [System.IO.File]::WriteAllText(
                $timelinePath, $expanded,
                [System.Text.UTF8Encoding]::new($false))

            # Normal means absence, not an undocumented fourth enum spelling.
            if ($null -eq $view.Value) {
                Remove-Item Env:MATTER_WATER_DIAGNOSTIC_VIEW -ErrorAction SilentlyContinue
            }

            $driveArguments = @(
                '-3', $drive,
                '--world', 'RiverFloatLab',
                '--timeline', $timelinePath,
                '--out-dir', $runDir,
                '--timeout', '4200',
                '--editor', $editor,
                '--hide-ui',
                '--env', 'MATTER_WINDOW_WIDTH=1280',
                '--env', 'MATTER_WINDOW_HEIGHT=720',
                '--env', 'MATTER_SUN_SHADOW_SAMPLES=16',
                '--env', "MATTER_WATER_CAPTURE_FRAME=$frame"
            )
            if ($null -ne $view.Value) {
                $driveArguments += '--env'
                $driveArguments += "MATTER_WATER_DIAGNOSTIC_VIEW=$($view.Value)"
            }
            Invoke-Checked -Description "Water continuity $prefix capture" -Command {
                & py @driveArguments
            }

            foreach ($cameraName in $cameraNames) {
                $png = Join-Path $captureDir $cameraName
                $done = "$png.done"
                if (-not (Test-Path -LiteralPath $png -PathType Leaf) -or
                    (Get-Item -LiteralPath $png).Length -eq 0) {
                    throw "Missing or empty water diagnostic PNG: $png"
                }
                if (-not (Test-Path -LiteralPath $done -PathType Leaf)) {
                    throw "Missing water diagnostic completion sidecar: $done"
                }
                # drive.py verified the fresh engine sentinel. Make the retained
                # evidence independently rejectable when copied elsewhere.
                [System.IO.File]::WriteAllText(
                    $done, "captured by water mesh continuity diagnostics`n",
                    [System.Text.UTF8Encoding]::new($false))
            }
        }
    }
}
finally {
    if ($hadPriorDiagnostic) {
        $env:MATTER_WATER_DIAGNOSTIC_VIEW = $priorDiagnostic
    } else {
        Remove-Item Env:MATTER_WATER_DIAGNOSTIC_VIEW -ErrorAction SilentlyContinue
    }
    Pop-Location
}

$pngs = @(Get-ChildItem -LiteralPath $diagnostics -Recurse -File -Filter '*.png')
$sidecars = @(Get-ChildItem -LiteralPath $diagnostics -Recurse -File -Filter '*.png.done')
if ($pngs.Count -ne 40 -or $sidecars.Count -ne 40) {
    throw "Expected 40 PNG/.done pairs, found $($pngs.Count) PNG and $($sidecars.Count) sidecars"
}
foreach ($file in @($pngs) + @($sidecars)) {
    if ($file.Length -eq 0) {
        throw "Retained water diagnostic artifact is empty: $($file.FullName)"
    }
}
Write-Output "Water mesh continuity diagnostics retained 40 PNG/.done pairs under $diagnostics"
