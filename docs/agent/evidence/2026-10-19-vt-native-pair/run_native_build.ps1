[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory
)
$ErrorActionPreference = 'Stop'
if ($SourceRoot.StartsWith('\\') -or -not [IO.Path]::IsPathRooted($SourceRoot)) {
    throw 'The authorized historical experiment requires an actual local drive root.'
}
New-Item -ItemType Directory -Path $EvidenceDirectory -ErrorAction Stop | Out-Null
Set-Location -LiteralPath $SourceRoot
$env:CMAKE_BUILD_PARALLEL_LEVEL = '6'
$wrapper = Join-Path $SourceRoot 'tools\build-windows.ps1'
$toolchain = & $wrapper -Config RelWithDebInfo -PreflightOnly | Out-String
$toolchain | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'toolchain.json')
$arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -Config RelWithDebInfo -Target vulkan_smoke_tests' -f $wrapper
$started = Get-Date
$receipt = [ordered]@{
    source_root = $SourceRoot
    command = "powershell.exe $arguments"
    working_directory = $SourceRoot
    started_at = $started.ToUniversalTime().ToString('o')
    timeout_seconds = 1800
    build_parallel_level = $env:CMAKE_BUILD_PARALLEL_LEVEL
    status = 'running'
    exit_code = $null
}
$receiptPath = Join-Path $EvidenceDirectory 'build-receipt.json'
$receipt | ConvertTo-Json | Set-Content -LiteralPath $receiptPath
$process = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') `
    -ArgumentList $arguments -WorkingDirectory $SourceRoot -PassThru `
    -RedirectStandardOutput (Join-Path $EvidenceDirectory 'build.stdout.txt') `
    -RedirectStandardError (Join-Path $EvidenceDirectory 'build.stderr.txt')
$null = $process.Handle
if (-not $process.WaitForExit(1800000)) {
    & taskkill.exe /PID $process.Id /T /F 2>&1 |
        Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'timeout-kill.txt')
    $receipt.status = 'timeout'
    $code = 124
} else {
    $process.WaitForExit()
    $code = $process.ExitCode
    if ($null -eq $code) { throw 'Native build exited without an observable exit code.' }
    $receipt.status = if ($code -eq 0) { 'completed' } else { 'failed' }
    $receipt.exit_code = $code
}
$receipt.elapsed_seconds = ((Get-Date) - $started).TotalSeconds
$receipt.finished_at = (Get-Date).ToUniversalTime().ToString('o')
$receipt | ConvertTo-Json | Set-Content -LiteralPath $receiptPath
$receipt | ConvertTo-Json
exit $code
