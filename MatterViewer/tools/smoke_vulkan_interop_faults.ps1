param(
    [string]$TestPath = (Join-Path $PSScriptRoot '..\build\windows\vulkan_smoke_tests.exe'),
    [int]$TimeoutMilliseconds = 15000
)

$ErrorActionPreference = 'Stop'
$TestPath = (Resolve-Path $TestPath).Path
$savedMode = $env:MATTER_VK_SMOKE_MODE
try {
    $modes = @(
        @{ Label = 'after-kernel-before-signal'; Mode = 'interop-fault-after-kernel-before-signal' },
        @{ Label = 'signal-enqueue-failure'; Mode = 'interop-fault-signal-enqueue-failure' },
        @{ Label = 'after-signal-before-vk-wait'; Mode = 'interop-fault-after-signal-before-vk-wait' },
        @{ Label = 'cuda-async-unproven'; Mode = 'interop-fault-cuda-async-unproven' },
        @{ Label = 'vk-wait-failure'; Mode = 'interop-fault-vk-wait-failure' },
        @{ Label = 'vk-recovery-unproven'; Mode = 'interop-fault-vk-recovery-unproven' },
        @{ Label = 'rt'; Mode = 'rt' },
        @{ Label = 'rt-disabled'; Mode = 'rt-disabled' },
        @{ Label = 'rt-unavailable'; Mode = 'rt-unavailable' }
    )
    foreach ($case in $modes) {
        $env:MATTER_VK_SMOKE_MODE = $case.Mode
        Write-Output "CUDA/Vulkan executable smoke: $($case.Label)"
        $process = $null
        try {
            $startInfo = New-Object System.Diagnostics.ProcessStartInfo
            $startInfo.FileName = $TestPath
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true

            # Windows environment names are case-insensitive, but an inherited
            # block can still contain both Path and PATH. The standard launcher with
            # redirected streams can throw while copying such a block. Rebuild
            # it with exactly one entry per case-insensitive name.
            $startInfo.EnvironmentVariables.Clear()
            $seenEnvironmentNames =
                [System.Collections.Generic.HashSet[string]]::new(
                    [System.StringComparer]::OrdinalIgnoreCase)
            $inheritedEnvironment = [Environment]::GetEnvironmentVariables()
            foreach ($rawName in $inheritedEnvironment.Keys) {
                $name = [string]$rawName
                if ($seenEnvironmentNames.Add($name)) {
                    $startInfo.EnvironmentVariables[$name] =
                        [string]$inheritedEnvironment[$rawName]
                }
            }

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
    Write-Output 'CUDA/Vulkan fault and RT smokes: PASS'
} finally {
    $env:MATTER_VK_SMOKE_MODE = $savedMode
}
