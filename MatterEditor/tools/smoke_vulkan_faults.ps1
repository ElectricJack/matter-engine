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
        @{ Label = 'rt-unavailable'; Mode = 'rt-unavailable' }
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
