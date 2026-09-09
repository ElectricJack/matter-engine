[CmdletBinding()]
param(
    [string]$OutputDir =
        'build/qa/water-mesh-continuity-2026-08-29/waterfall-matrix'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$outputPath = if ([System.IO.Path]::IsPathRooted($OutputDir)) {
    [System.IO.Path]::GetFullPath($OutputDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $OutputDir))
}
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null

$modulePath = Join-Path $repositoryRoot `
    'tools\windows\MatterWindowsToolchain.psm1'
Import-Module $modulePath -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot
$python = $toolchain.Python
$pythonLauncherArguments = if (
    [System.IO.Path]::GetFileName($python) -ieq 'py.exe') {
    @('-3.13')
} else {
    @()
}

function Invoke-CheckedPython {
    param(
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][string[]]$PythonArguments
    )
    $invokeArguments = @($pythonLauncherArguments) + @($PythonArguments)
    & $python @invokeArguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

Push-Location $repositoryRoot
try {
    & (Join-Path $repositoryRoot 'tools\build-windows.ps1') `
        -Config RelWithDebInfo -Target vulkan_smoke_tests
    if ($LASTEXITCODE -ne 0) {
        throw "vulkan_smoke_tests build failed with exit code $LASTEXITCODE"
    }

    Invoke-CheckedPython -Description 'waterfall quality parser tests' `
        -PythonArguments @(
            '-m', 'unittest',
            'MatterEngine3.tools.tests.test_waterfall_visual_quality', '-v')

    $executable = Join-Path $repositoryRoot `
        'MatterEditor\build\cmake\windows-msvc\relwithdebinfo\vulkan_smoke_tests.exe'
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Vulkan smoke executable was not produced: $executable"
    }
    $comparator = Join-Path $repositoryRoot `
        'MatterEngine3\tools\waterfall_visual_quality.py'
    $reports = @()
    $previousMode = $env:MATTER_VK_SMOKE_MODE
    $previousReport = $env:MATTER_WATERFALL_QUALITY_REPORT
    try {
        for ($run = 1; $run -le 3; ++$run) {
            $report = Join-Path $outputPath "run-$run.json"
            $summary = Join-Path $outputPath "run-$run-summary.json"
            $log = Join-Path $outputPath "run-$run.log"
            foreach ($stale in @($report, $summary, $log)) {
                if (Test-Path -LiteralPath $stale) {
                    Remove-Item -LiteralPath $stale -Force
                }
            }
            $env:MATTER_VK_SMOKE_MODE = 'waterfall-mesher'
            $env:MATTER_WATERFALL_QUALITY_REPORT = $report
            $nativeOutput = & $executable 2>&1
            $nativeExitCode = $LASTEXITCODE
            $nativeOutput | Set-Content -LiteralPath $log -Encoding utf8
            $nativeOutput | ForEach-Object { Write-Host $_ }
            if ($nativeExitCode -ne 0) {
                throw "waterfall matrix run $run failed with exit code " +
                    "$nativeExitCode; see $log"
            }
            if (-not (Test-Path -LiteralPath $report -PathType Leaf) -or
                (Get-Item -LiteralPath $report).Length -eq 0) {
                throw "waterfall matrix run $run did not produce $report"
            }
            Invoke-CheckedPython `
                -Description "waterfall matrix run $run parser" `
                -PythonArguments @($comparator, $report, '--summary', $summary)
            $reports += $report
        }
    } finally {
        $env:MATTER_VK_SMOKE_MODE = $previousMode
        $env:MATTER_WATERFALL_QUALITY_REPORT = $previousReport
    }

    $decision = Join-Path $outputPath 'decision.json'
    $markdown = Join-Path $outputPath 'report.md'
    Invoke-CheckedPython -Description 'three-run waterfall determinism gate' `
        -PythonArguments @(
            $comparator, $reports[0], $reports[1], $reports[2],
            '--summary', $decision, '--markdown', $markdown)
    Write-Output "MATTER_WATERFALL_QUALITY_DECISION=$decision"
    Write-Output "MATTER_WATERFALL_QUALITY_REPORT=$markdown"
} finally {
    Pop-Location
}
