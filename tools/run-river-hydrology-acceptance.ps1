[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PhysxRoot,
    [Parameter(Mandatory = $true)][string]$CudaRoot,
    [string]$RunId = (Get-Date -Format 'yyyyMMdd-HHmmss')
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Resolve-RequiredDirectory([string]$Path, [string]$Label) {
    if (-not [System.IO.Path]::IsPathRooted($Path)) {
        throw "$Label must be an absolute path: $Path"
    }
    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
        throw "$Label is not a directory: $resolved"
    }
    return $resolved
}

function Read-KeyValueFile([string]$Path) {
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        $pair = $line -split '=', 2
        if ($pair.Count -eq 2) { $values[$pair[0]] = $pair[1] }
    }
    return $values
}

$resolvedPhysx = Resolve-RequiredDirectory $PhysxRoot 'PhysX root'
$resolvedCuda = Resolve-RequiredDirectory $CudaRoot 'CUDA root'
if ($RunId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
    throw 'RunId may contain only letters, digits, dot, underscore, and hyphen.'
}

$runParent = Join-Path $repositoryRoot 'MatterEditor\build\baselines\msvc\physx-river-sections'
$runRoot = [System.IO.Path]::GetFullPath((Join-Path $runParent $RunId))
$expectedParent = [System.IO.Path]::GetFullPath($runParent)
if ([System.IO.Path]::GetDirectoryName($runRoot) -ne $expectedParent) {
    throw "Resolved run directory escaped its expected parent: $runRoot"
}
if (Test-Path -LiteralPath $runRoot) {
    throw "Acceptance run directory already exists: $runRoot"
}
$screenshotsRoot = Join-Path $runRoot 'screenshots'
$traceRoot = Join-Path $runRoot 'trace'
New-Item -ItemType Directory -Path $screenshotsRoot -Force | Out-Null
New-Item -ItemType Directory -Path $traceRoot -Force | Out-Null

& (Join-Path $repositoryRoot 'tools\build-windows.ps1') `
    -Config RelWithDebInfo -Target matter_editor -EnablePhysx `
    -PhysxRoot $resolvedPhysx -CudaRoot $resolvedCuda
if ($LASTEXITCODE -ne 0) { throw "matter_editor build failed with exit code $LASTEXITCODE" }

$cacheParent = [System.IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot 'projects\world_demo\.cache'))
$cacheTarget = [System.IO.Path]::GetFullPath(
    (Join-Path $cacheParent 'RiverHydrology'))
if ([System.IO.Path]::GetDirectoryName($cacheTarget) -ne $cacheParent -or
    [System.IO.Path]::GetFileName($cacheTarget) -ne 'RiverHydrology') {
    throw "Refusing to clear unexpected cache target: $cacheTarget"
}
if (Test-Path -LiteralPath $cacheTarget) {
    Remove-Item -LiteralPath $cacheTarget -Recurse -Force
}

$timelineTemplate = Join-Path $repositoryRoot 'MatterEngine3\tools\river_hydrology_sections.timeline'
$timeline = Join-Path $runRoot 'timeline.txt'
$timelineOutput = $screenshotsRoot.Replace('\', '/')
$expanded = (Get-Content -LiteralPath $timelineTemplate -Raw).Replace(
    '{{OUTPUT_DIR}}', $timelineOutput)
[System.IO.File]::WriteAllText($timeline, $expanded,
    [System.Text.UTF8Encoding]::new($false))

$editor = Join-Path $repositoryRoot 'MatterEditor\build\windows-msvc\editor.exe'
$drive = Join-Path $repositoryRoot 'MatterEngine3\tools\drive.py'
& py -3 $drive --world RiverHydrology --timeline $timeline `
    --out-dir $runRoot --timeout 1800 --editor $editor `
    --env "MATTER_HYDROLOGY_TRACE_DIR=$traceRoot"
if ($LASTEXITCODE -ne 0) { throw "RiverHydrology editor run failed with exit code $LASTEXITCODE" }

$shotNames = @(
    'overview.png', 'upper-rapids.png', 'waterfall-approach.png',
    'waterfall-side.png', 'plunge-pool.png', 'spillway.png',
    'lower-rapids.png', 'second-pool.png', 'player-low.png'
)
foreach ($name in $shotNames) {
    $png = Join-Path $screenshotsRoot $name
    $done = "$png.done"
    if (-not (Test-Path -LiteralPath $png -PathType Leaf) -or
        (Get-Item -LiteralPath $png).Length -eq 0 -or
        -not (Test-Path -LiteralPath $done -PathType Leaf)) {
        throw "Acceptance screenshot or completion sidecar is missing: $png"
    }
}

$timingPath = Join-Path $traceRoot 'timings.json'
if (-not (Test-Path -LiteralPath $timingPath -PathType Leaf)) {
    throw "Network timing trace is missing: $timingPath"
}
$timings = Get-Content -LiteralPath $timingPath -Raw | ConvertFrom-Json
if ($timings.networkState -ne 'Ready' -or $timings.sections.Count -ne 2 -or
    $timings.handoffMeshMs -le 0 -or $timings.serializeMs -le 0 -or
    $timings.totalWallMs -le 0 -or $timings.visualVertices -le 0 -or
    $timings.visualTriangles -le 0) {
    throw 'Network timing trace is incomplete or did not reach Ready.'
}

foreach ($section in $timings.sections) {
    if ($section.cacheHit -or $section.setupMs -le 0 -or
        $section.physxInitMs -le 0 -or $section.simulateMs -le 0 -or
        $section.gpuMeshMs -le 0 -or $section.cpuMeshMs -le 0 -or
        $section.particles -le 0 -or $section.escaped -ne 0) {
        throw "Section timing or particle acceptance is invalid: $($section.id)"
    }
    $metadataPath = Join-Path (Join-Path $traceRoot $section.id) 'metadata.txt'
    if (-not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) {
        throw "Section metadata is missing: $metadataPath"
    }
    $metadata = Read-KeyValueFile $metadataPath
    if ($metadata.accepted -ne 'true' -or [int]$metadata.terminal_code -ne 0 -or
        [int]$metadata.escaped -gt [int]$metadata.escape_budget -or
        [double]$metadata.sensor_final -le 0) {
        throw "Section metadata did not satisfy acceptance: $($section.id)"
    }
}

$summary = [ordered]@{
    runId = $RunId
    networkState = $timings.networkState
    sections = $timings.sections
    handoffMeshMs = $timings.handoffMeshMs
    serializeMs = $timings.serializeMs
    totalWallMs = $timings.totalWallMs
    visualVertices = $timings.visualVertices
    visualTriangles = $timings.visualTriangles
    screenshots = $shotNames
    traceDirectory = $traceRoot
}
$summaryPath = Join-Path $runRoot 'summary.json'
[System.IO.File]::WriteAllText(
    $summaryPath, ($summary | ConvertTo-Json -Depth 6),
    [System.Text.UTF8Encoding]::new($false))
Write-Output "RIVER_HYDROLOGY_ACCEPTANCE=$runRoot"
Write-Output "RIVER_HYDROLOGY_SUMMARY=$summaryPath"
