[CmdletBinding()]
param(
    [ValidateSet('Stage1')]
    [string]$Stage = 'Stage1',

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

function Invoke-EditorRun {
    param(
        [Parameter(Mandatory = $true)][string]$RunDir,
        [Parameter(Mandatory = $true)][string]$Timeline,
        [string[]]$Environment = @()
    )
    $arguments = @(
        $drive,
        '--world', 'RiverFloatLab',
        '--timeline', $Timeline,
        '--out-dir', $RunDir,
        '--timeout', '4200',
        '--editor', $fixtureEditor,
        '--hide-ui',
        '--env', 'MATTER_WINDOW_WIDTH=1280',
        '--env', 'MATTER_WINDOW_HEIGHT=720',
        '--env', 'MATTER_SUN_SHADOW_SAMPLES=16',
        '--env', "PATH=$nativeDllPath"
    )
    foreach ($entry in $Environment) {
        $arguments += '--env'
        $arguments += $entry
    }
    Invoke-Checked -Description "RiverFloatLab run $RunDir" -Command {
        & $python @arguments
    }
}

if ($Stage -ne 'Stage1') {
    throw "Only Stage1 acceptance is implemented"
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$outputRoot = Resolve-OutputPath $OutputDir
if (Test-Path -LiteralPath $outputRoot) {
    $existing = @(Get-ChildItem -LiteralPath $outputRoot -Force)
    if ($existing.Count -ne 0) {
        throw "Stage1 output must be a new or empty directory: $outputRoot"
    }
}
New-Item -ItemType Directory -Force $outputRoot | Out-Null

$toolchainJson = & (Join-Path $repoRoot 'tools/build-windows.ps1') `
    -PreflightOnly | Out-String
if ($LASTEXITCODE -ne 0) {
    throw 'Windows toolchain preflight failed'
}
$toolchain = $toolchainJson | ConvertFrom-Json
$python = $toolchain.Python
if (-not (Test-Path -LiteralPath $python -PathType Leaf)) {
    throw "Native Windows Python was not found: $python"
}

$sourceProject = Join-Path $repoRoot 'projects/world_demo'
$sourceEngineShared = Join-Path $repoRoot 'MatterEngine3/shared-lib'
$sourceEditor = Join-Path $repoRoot 'MatterEditor/build/windows-msvc/editor.exe'
$cmakeCache = Join-Path $repoRoot `
    'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/CMakeCache.txt'
$drive = Join-Path $repoRoot 'MatterEngine3/tools/drive.py'
$comparator = Join-Path $repoRoot 'MatterEngine3/tools/water_mesh_continuity_acceptance.py'
$captureTemplate = Join-Path $repoRoot 'MatterEngine3/tools/water_mesh_continuity_acceptance.timeline'
$smoke = Join-Path $repoRoot 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe'
foreach ($required in @($sourceProject, $sourceEngineShared, $sourceEditor,
                         $cmakeCache, $drive, $comparator, $captureTemplate,
                         $smoke)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required Stage1 input is missing: $required"
    }
}
$configuredFeatures = Get-Content -LiteralPath $cmakeCache -Raw
if ($configuredFeatures -notmatch '(?m)^MATTER_ENABLE_PHYSX:BOOL=ON\r?$') {
    throw 'Stage1 requires the MSVC editor preset configured with MATTER_ENABLE_PHYSX=ON'
}

# The executable searches upward from its own location before the repository.
# A tiny fixture tree therefore isolates both the authored downstream edit and
# the real per-world cache without touching the user's project or its cache.
$fixtureRoot = Join-Path $outputRoot 'fixture-root'
$fixtureProject = Join-Path $fixtureRoot 'projects/world_demo'
$fixtureEngineShared = Join-Path $fixtureRoot 'MatterEngine3/shared-lib'
$fixtureBin = Join-Path $fixtureRoot 'bin'
$fixtureEditor = Join-Path $fixtureBin 'editor.exe'
New-Item -ItemType Directory -Force `
    $fixtureProject, $fixtureEngineShared, $fixtureBin | Out-Null
Get-ChildItem -LiteralPath $sourceProject -Force |
    Where-Object { $_.Name -ne '.cache' } |
    Copy-Item -Destination $fixtureProject -Recurse -Force
Copy-Item -Path (Join-Path $sourceEngineShared '*') `
    -Destination $fixtureEngineShared -Recurse -Force
Copy-Item -LiteralPath $sourceEditor -Destination $fixtureEditor -Force

$nativeConfig = Join-Path $repoRoot 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo'
$nativePackage = Join-Path $repoRoot 'MatterEditor/build/windows-msvc'
$nativeDllPath = "$nativeConfig;$nativePackage;$env:PATH"
$bakeTimeline = Join-Path $outputRoot 'bake-only.timeline'
[System.IO.File]::WriteAllText(
    $bakeTimeline,
    "wait_event bake.finished 3600`nquit`n",
    [System.Text.UTF8Encoding]::new($false))
$hadPriorWaterDiagnostic = Test-Path Env:MATTER_WATER_DIAGNOSTIC_VIEW
$priorWaterDiagnostic = if ($hadPriorWaterDiagnostic) {
    $env:MATTER_WATER_DIAGNOSTIC_VIEW
} else {
    $null
}

Push-Location $repoRoot
try {
    foreach ($run in @('cold', 'cache')) {
        $runRoot = Join-Path $outputRoot $run
        $trace = Join-Path $runRoot 'trace'
        New-Item -ItemType Directory -Force $runRoot | Out-Null
        Invoke-EditorRun -RunDir (Join-Path $runRoot 'editor') `
            -Timeline $bakeTimeline `
            -Environment @("MATTER_HYDROLOGY_TRACE_DIR=$trace")
        $timings = Join-Path $trace 'timings.json'
        if (-not (Test-Path -LiteralPath $timings -PathType Leaf) -or
            (Get-Item -LiteralPath $timings).Length -eq 0) {
            throw "$run did not produce a nonempty hydrology timing trace"
        }
    }

    $screenshots = Join-Path $outputRoot 'screenshots'
    $template = Get-Content -Raw $captureTemplate
    $frames = 0, 7, 15, 22, 29
    $views = @(
        @{ Name = 'normal'; Value = $null },
        @{ Name = 'geometry-normal'; Value = 'geometry-normal' },
        @{ Name = 'foam-driver'; Value = 'foam-driver' },
        @{ Name = 'identity'; Value = 'identity' }
    )
    foreach ($frame in $frames) {
        foreach ($view in $views) {
            $tag = 'frame-{0:D2}-{1}' -f $frame, $view.Name
            $captureDir = Join-Path $screenshots $tag
            $runDir = Join-Path $outputRoot "capture-$tag"
            New-Item -ItemType Directory -Force $captureDir, $runDir | Out-Null
            $captureToken = $captureDir.Replace('\', '/')
            $timeline = Join-Path $runDir 'capture.timeline'
            [System.IO.File]::WriteAllText(
                $timeline, $template.Replace('{{OUTPUT_DIR}}', $captureToken),
                [System.Text.UTF8Encoding]::new($false))
            $captureEnvironment = @("MATTER_WATER_CAPTURE_FRAME=$frame")
            if ($null -ne $view.Value) {
                $captureEnvironment +=
                    "MATTER_WATER_DIAGNOSTIC_VIEW=$($view.Value)"
            } else {
                # Normal shading is selected by absence of the diagnostic
                # variable. An empty value is an invalid diagnostic enum.
                Remove-Item Env:MATTER_WATER_DIAGNOSTIC_VIEW `
                    -ErrorAction SilentlyContinue
            }
            Invoke-EditorRun -RunDir $runDir -Timeline $timeline `
                -Environment $captureEnvironment
            $png = Join-Path $captureDir 'section-handoff.png'
            $done = "$png.done"
            if (-not (Test-Path -LiteralPath $png -PathType Leaf) -or
                (Get-Item -LiteralPath $png).Length -eq 0 -or
                -not (Test-Path -LiteralPath $done -PathType Leaf) -or
                (Get-Item -LiteralPath $done).Length -eq 0) {
                throw "Missing Stage1 screenshot pair: $png"
            }
        }
    }

    $fixtureDefinition = Join-Path `
        $fixtureProject 'shared-lib/river_hydrology_definition.js'
    $definition = Get-Content -LiteralPath $fixtureDefinition -Raw
    $needle = 'from: secondSpillway - 22, to: secondSpillway, fillLevel: 28,'
    $replacement = 'from: secondSpillway - 22, to: secondSpillway, fillLevel: 28.25,'
    $matches = ([regex]::Matches($definition, [regex]::Escape($needle))).Count
    if ($matches -ne 1) {
        throw "Fixture-only downstream edit expected one match, found $matches"
    }
    [System.IO.File]::WriteAllText(
        $fixtureDefinition, $definition.Replace($needle, $replacement),
        [System.Text.UTF8Encoding]::new($false))
    $editRoot = Join-Path $outputRoot 'edit'
    $editTrace = Join-Path $editRoot 'trace'
    New-Item -ItemType Directory -Force $editRoot | Out-Null
    Invoke-EditorRun -RunDir (Join-Path $editRoot 'editor') `
        -Timeline $bakeTimeline `
        -Environment @("MATTER_HYDROLOGY_TRACE_DIR=$editTrace")
    $editTimings = Join-Path $editTrace 'timings.json'
    if (-not (Test-Path -LiteralPath $editTimings -PathType Leaf) -or
        (Get-Item -LiteralPath $editTimings).Length -eq 0) {
        throw 'downstream edit did not produce a nonempty hydrology timing trace'
    }

    $nativeRoot = Join-Path $outputRoot 'native'
    New-Item -ItemType Directory -Force $nativeRoot | Out-Null
    $priorSmokeMode = if (Test-Path Env:MATTER_VK_SMOKE_MODE) {
        $env:MATTER_VK_SMOKE_MODE
    } else { $null }
    try {
        foreach ($mode in @('gpu-mesher', 'water-forward',
                            'water-animation', 'default')) {
            if ($mode -eq 'default') {
                Remove-Item Env:MATTER_VK_SMOKE_MODE -ErrorAction SilentlyContinue
            } else {
                $env:MATTER_VK_SMOKE_MODE = $mode
            }
            $log = Join-Path $nativeRoot "$mode.log"
            # Vulkan loader diagnostics use stderr even when the smoke test
            # succeeds. Do not let PowerShell promote those lines into a
            # terminating NativeCommandError before the real exit code is
            # inspected and the full log is retained.
            $priorNativeErrorPreference = $ErrorActionPreference
            try {
                $ErrorActionPreference = 'Continue'
                & $smoke 2>&1 | Tee-Object -FilePath $log
                $smokeExitCode = $LASTEXITCODE
            } finally {
                $ErrorActionPreference = $priorNativeErrorPreference
            }
            if ($smokeExitCode -ne 0) {
                throw "Vulkan smoke mode $mode failed with exit code $smokeExitCode"
            }
        }
    } finally {
        if ($null -eq $priorSmokeMode) {
            Remove-Item Env:MATTER_VK_SMOKE_MODE -ErrorAction SilentlyContinue
        } else {
            $env:MATTER_VK_SMOKE_MODE = $priorSmokeMode
        }
    }

    $validationErrors = 0
    foreach ($log in Get-ChildItem -LiteralPath $nativeRoot -Filter '*.log') {
        $text = Get-Content -LiteralPath $log.FullName -Raw
        foreach ($match in [regex]::Matches($text, 'validation errors:\s*(\d+)')) {
            $validationErrors = [Math]::Max(
                $validationErrors, [int]$match.Groups[1].Value)
        }
    }
    $animationLog = Get-Content -LiteralPath `
        (Join-Path $nativeRoot 'water-animation.log') -Raw
    $rt = [regex]::Match(
        $animationLog,
        'water animation RT counters: decode=(\d+) blas=(\d+) tlas_before=(\d+) tlas_after=(\d+) records=(\d+)')
    if (-not $rt.Success) {
        throw 'water-animation smoke did not report RT counters'
    }
    $rasterDirectDraws = [regex]::Match(
        $animationLog, 'water animation raster direct draws: (\d+)')
    if (-not $rasterDirectDraws.Success -or
        [uint32]$rasterDirectDraws.Groups[1].Value -ne 1) {
        throw 'water-animation smoke did not prove the raster direct draw'
    }
    $nativeGates = [ordered]@{
        validationErrors = $validationErrors
        waterDecodeDispatches = [uint64]$rt.Groups[1].Value
        waterBlasBuilds = [uint32]$rt.Groups[2].Value
        waterTlasInstances =
            [uint64]$rt.Groups[4].Value - [uint64]$rt.Groups[3].Value
        waterRtRecords = [uint64]$rt.Groups[5].Value
        rasterDirectDraws = [uint32]$rasterDirectDraws.Groups[1].Value
    }
    $nativeJson = Join-Path $nativeRoot 'native-gates.json'
    [System.IO.File]::WriteAllText(
        $nativeJson, ($nativeGates | ConvertTo-Json) + "`n",
        [System.Text.UTF8Encoding]::new($false))

    $summary = Join-Path $outputRoot 'stage1-summary.json'
    Invoke-Checked -Description 'Animated-water Stage1 comparator' -Command {
        & $python $comparator stage1 `
            --cold (Join-Path $outputRoot 'cold/trace/timings.json') `
            --cache (Join-Path $outputRoot 'cache/trace/timings.json') `
            --edit $editTimings `
            --native $nativeJson `
            --screenshots $screenshots `
            --output $summary
    }
}
finally {
    if ($hadPriorWaterDiagnostic) {
        $env:MATTER_WATER_DIAGNOSTIC_VIEW = $priorWaterDiagnostic
    } else {
        Remove-Item Env:MATTER_WATER_DIAGNOSTIC_VIEW `
            -ErrorAction SilentlyContinue
    }
    Pop-Location
}

Write-Output "Animated-water Stage1 evidence retained under $outputRoot"
