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
        $stdout = Join-Path $env:TEMP ("matter-vk-smoke-" + [guid]::NewGuid().ToString('N') + '.out')
        $stderr = "$stdout.err"
        try {
            $process = Start-Process -FilePath $TestPath -NoNewWindow -PassThru `
                -RedirectStandardOutput $stdout -RedirectStandardError $stderr
            # Materialize the process handle before waiting; Windows PowerShell can
            # otherwise lose ExitCode when a short-lived child exits immediately.
            $null = $process.Handle
            if (-not $process.WaitForExit($TimeoutMilliseconds)) {
                $process.Kill()
                throw "$($case.Label) exceeded the bounded $TimeoutMilliseconds ms gate"
            }
            $process.Refresh()
            $log = @()
            if (Test-Path $stdout) { $log += Get-Content $stdout }
            if (Test-Path $stderr) { $log += Get-Content $stderr }
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
            Remove-Item -LiteralPath $stdout,$stderr -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Output 'CUDA/Vulkan fault and RT smokes: PASS'
} finally {
    $env:MATTER_VK_SMOKE_MODE = $savedMode
}
