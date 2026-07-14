param(
    [string]$TestPath = (Join-Path $PSScriptRoot '..\build\windows\vulkan_smoke_tests.exe'),
    [int]$TimeoutMilliseconds = 15000
)

$ErrorActionPreference = 'Stop'
$TestPath = (Resolve-Path $TestPath).Path
$savedMode = $env:MATTER_VK_SMOKE_MODE
try {
    $modes = @(
        'after-kernel-before-signal',
        'signal-enqueue-failure',
        'after-signal-before-vk-wait',
        'cuda-async-unproven',
        'vk-wait-failure',
        'vk-recovery-unproven'
    )
    foreach ($mode in $modes) {
        $env:MATTER_VK_SMOKE_MODE = "interop-fault-$mode"
        Write-Output "CUDA/Vulkan fault smoke: $mode"
        $process = Start-Process -FilePath $TestPath -NoNewWindow -PassThru
        # Materialize the process handle before waiting; Windows PowerShell can
        # otherwise lose ExitCode when a short-lived child exits immediately.
        $null = $process.Handle
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            $process.Kill()
            throw "$mode exceeded the bounded $TimeoutMilliseconds ms gate"
        }
        $process.Refresh()
        if ($process.ExitCode -ne 0) {
            throw "$mode exited with code $($process.ExitCode)"
        }
    }
    Write-Output 'CUDA/Vulkan fault smokes: PASS'
} finally {
    $env:MATTER_VK_SMOKE_MODE = $savedMode
}
