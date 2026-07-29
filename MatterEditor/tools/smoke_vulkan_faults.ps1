param(
    [string]$TestPath = (Join-Path $PSScriptRoot '..\build\windows\vulkan_smoke_tests.exe'),
    [int]$TimeoutMilliseconds = 15000
)
$ErrorActionPreference = 'Stop'
$TestPath = (Resolve-Path $TestPath).Path
$savedMode = [Environment]::GetEnvironmentVariable(
    'MATTER_VK_SMOKE_MODE', [EnvironmentVariableTarget]::Process)
try {
    $modes = @(
        @{ Label = 'Streamline missing instance proxy'; Mode = 'streamline-missing-instance-proxy' },
        @{ Label = 'Streamline missing device proxy'; Mode = 'streamline-missing-device-proxy' },
        @{ Label = 'rt'; Mode = 'rt' },
        @{ Label = 'rt-disabled'; Mode = 'rt-disabled' },
        @{ Label = 'rt-unavailable'; Mode = 'rt-unavailable' },
        @{ Label = 'animation skin compute readback'; Mode = 'animation-skin' },
        # WP-E: chart-space virtual texturing residency + stub filler +
        # G-buffer sampling (pages, seams, chartless regression gate).
        @{ Label = 'chart-space virtual texturing'; Mode = 'vt' },
        # WP-F: surfaces() classifier tape driving VT page composition
        # (tape regions, determinism, tape-edit invalidation, strip fallback).
        @{ Label = 'chart VT surfaces() tape'; Mode = 'vt-surfaces' },
        # WP-G: RT sampling of VT pages + ray cones (G-buffer/traced-hit
        # consistency, cone-mip monotonicity).
        @{ Label = 'chart VT in the RT path'; Mode = 'vt-rt' },
        # WP-H: tier-2 hemisphere AO page enrichment (occluder fixture,
        # determinism, no double-application, invalidation re-enrich) and the
        # same fixture on a device forced to report no ray tracing, where the
        # tier must be absent and pages must read exactly as they did before.
        @{ Label = 'chart VT tier-2 hemisphere AO'; Mode = 'vt-enrich' },
        @{ Label = 'chart VT tier-2 without ray tracing'; Mode = 'vt-enrich-nort' }
    )
    foreach ($case in $modes) {
        [Environment]::SetEnvironmentVariable('MATTER_VK_SMOKE_MODE',
            $case.Mode, [EnvironmentVariableTarget]::Process)
        Write-Output "Vulkan executable smoke: $($case.Label)"
        $process = $null
        try {
            $startInfo = New-Object System.Diagnostics.ProcessStartInfo
            $startInfo.FileName = $TestPath
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true

            # Do not access either managed ProcessStartInfo environment
            # dictionary. On Windows PowerShell 5.1 their initialization is
            # fragile when the raw inherited block contains both Path and PATH.
            # The child inherits that raw block and the per-case mode set above.

            $process = New-Object System.Diagnostics.Process
            $process.StartInfo = $startInfo
            if (-not $process.Start()) {
                throw "$($case.Label) process did not start"
            }
            $stdoutTask = $process.StandardOutput.ReadToEndAsync()
            $stderrTask = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit($TimeoutMilliseconds)) {
                $process.Kill()
                $process.WaitForExit()
                throw "$($case.Label) exceeded the bounded $TimeoutMilliseconds ms gate"
            }
            $log = @(($stdoutTask.Result -split "`r?`n") +
                     ($stderrTask.Result -split "`r?`n")) |
                Where-Object { $_ -ne '' }
            $log | ForEach-Object { Write-Output $_ }
            $joined = $log -join "`n"
            if ($process.ExitCode -ne 0) {
                throw "$($case.Label) exited with code $($process.ExitCode)"
            }
            if ($joined -notmatch 'validation errors: 0') {
                throw "$($case.Label) did not report validation errors 0"
            }
            if ($joined -notmatch 'ALL PASS') {
                throw "$($case.Label) did not report ALL PASS"
            }
        } finally {
            if ($process) { $process.Dispose() }
        }
    }
    Write-Output 'Vulkan fault and RT smokes: PASS'
} finally {
    [Environment]::SetEnvironmentVariable('MATTER_VK_SMOKE_MODE',
        $savedMode, [EnvironmentVariableTarget]::Process)
}
