[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [Parameter(Mandatory = $true)][string]$ToolchainReceipt,
    [Parameter(Mandatory = $true)][ValidateSet('vt', 'vt-rt')][string]$Mode
)
$ErrorActionPreference = 'Stop'
if ($SourceRoot.StartsWith('\\') -or -not [IO.Path]::IsPathRooted($SourceRoot)) {
    throw 'Native historical tests require an actual local drive source root.'
}
New-Item -ItemType Directory -Path $EvidenceDirectory -ErrorAction Stop | Out-Null
$toolchain = Get-Content -LiteralPath $ToolchainReceipt -Raw | ConvertFrom-Json
$env:MATTER_VK_SMOKE_MODE = $Mode
$env:VK_LAYER_PATH = Join-Path $toolchain.VulkanSdk 'Bin'
$sourceEditor = Join-Path $SourceRoot 'MatterEditor'
$executable = Join-Path $sourceEditor 'build\cmake\windows-msvc\relwithdebinfo\vulkan_smoke_tests.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing fresh executable: $executable" }
$started = Get-Date
$receipt = [ordered]@{
    source_root = $SourceRoot
    working_directory = $sourceEditor
    executable = $executable
    executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    mode = $Mode
    validation_layer_path = $env:VK_LAYER_PATH
    started_at = $started.ToUniversalTime().ToString('o')
    timeout_seconds = 240
    status = 'running'
    exit_code = $null
}
$receiptPath = Join-Path $EvidenceDirectory 'smoke-receipt.json'
$receipt | ConvertTo-Json | Set-Content -LiteralPath $receiptPath
$process = Start-Process -FilePath $executable -WorkingDirectory $sourceEditor -PassThru `
    -RedirectStandardOutput (Join-Path $EvidenceDirectory 'smoke.stdout.txt') `
    -RedirectStandardError (Join-Path $EvidenceDirectory 'smoke.stderr.txt')
$null = $process.Handle
if (-not $process.WaitForExit(240000)) {
    & taskkill.exe /PID $process.Id /T /F 2>&1 |
        Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'timeout-kill.txt')
    $receipt.status = 'timeout'
    $code = 124
} else {
    $process.WaitForExit()
    $code = $process.ExitCode
    if ($null -eq $code) { throw 'Native smoke exited without an observable exit code.' }
    $receipt.status = 'completed'
}
$receipt.exit_code = $code
$receipt.elapsed_seconds = ((Get-Date) - $started).TotalSeconds
$receipt.finished_at = (Get-Date).ToUniversalTime().ToString('o')
$receipt | ConvertTo-Json | Set-Content -LiteralPath $receiptPath
$receipt | ConvertTo-Json
exit $code
