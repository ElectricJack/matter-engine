[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [Parameter(Mandatory = $true)][string]$VulkanSdk,
    [Parameter(Mandatory = $true)][ValidateSet('vt', 'vt-rt')][string]$Mode,
    [ValidateRange(1, 3600)][int]$TimeoutSeconds = 240
)
# Same receipt as the epoch-15/16 run_native_smoke.ps1, but launched the way
# tools/run-windows-build-bounded.ps1 launches builds: CreateNoWindow, stdin
# closed, so no visible console can deliver CTRL_CLOSE/CTRL_C (0xC000013A).
$ErrorActionPreference = 'Stop'
if ($SourceRoot.StartsWith('\\') -or -not [IO.Path]::IsPathRooted($SourceRoot)) {
    throw 'Native historical tests require an actual local drive source root.'
}
New-Item -ItemType Directory -Path $EvidenceDirectory -ErrorAction Stop | Out-Null
$EvidenceDirectory = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$sourceEditor = Join-Path $SourceRoot 'MatterEditor'
$executable = Join-Path $sourceEditor 'build\cmake\windows-msvc\relwithdebinfo\vulkan_smoke_tests.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing fresh executable: $executable" }
$layerPath = Join-Path $VulkanSdk 'Bin'
[Environment]::SetEnvironmentVariable('MATTER_VK_SMOKE_MODE', $Mode, [EnvironmentVariableTarget]::Process)
[Environment]::SetEnvironmentVariable('VK_LAYER_PATH', $layerPath, [EnvironmentVariableTarget]::Process)
$started = Get-Date
$receipt = [ordered]@{
    source_root = $SourceRoot
    working_directory = $sourceEditor
    executable = $executable
    executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    mode = $Mode
    validation_layer_path = $layerPath
    console = 'CreateNoWindow'
    started_at = $started.ToUniversalTime().ToString('o')
    timeout_seconds = $TimeoutSeconds
    status = 'running'
    exit_code = $null
}
$receiptPath = Join-Path $EvidenceDirectory 'smoke-receipt.json'
$receipt | ConvertTo-Json | Set-Content -LiteralPath $receiptPath
$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $executable
$startInfo.WorkingDirectory = $sourceEditor
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$stdout = [IO.File]::Open((Join-Path $EvidenceDirectory 'smoke.stdout.txt'), 'CreateNew', 'Write', 'Read')
$stderr = [IO.File]::Open((Join-Path $EvidenceDirectory 'smoke.stderr.txt'), 'CreateNew', 'Write', 'Read')
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $startInfo
try {
    if (-not $process.Start()) { throw 'Native smoke process did not start.' }
    $receipt.process_id = $process.Id
    $process.StandardInput.Close()
    $stdoutCopy = $process.StandardOutput.BaseStream.CopyToAsync($stdout)
    $stderrCopy = $process.StandardError.BaseStream.CopyToAsync($stderr)
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $ErrorActionPreference = 'Continue'
        & taskkill.exe /PID $process.Id /T /F 2>&1 | ForEach-Object { "$_" } |
            Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'timeout-kill.txt')
        $ErrorActionPreference = 'Stop'
        $process.WaitForExit()
        $receipt.status = 'timeout'
        $code = 124
    } else {
        $process.WaitForExit()
        $code = $process.ExitCode
        $receipt.status = 'completed'
    }
    $null = [Threading.Tasks.Task]::WaitAll(@($stdoutCopy, $stderrCopy), 30000)
} finally {
    $stdout.Dispose()
    $stderr.Dispose()
    $process.Dispose()
}
$receipt.exit_code = $code
$receipt.elapsed_seconds = ((Get-Date) - $started).TotalSeconds
$receipt.finished_at = (Get-Date).ToUniversalTime().ToString('o')
$receipt | ConvertTo-Json | Set-Content -LiteralPath $receiptPath
$receipt | ConvertTo-Json
exit $code
