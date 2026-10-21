[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [string]$SourceRoot = (Join-Path $PSScriptRoot '..'),
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 1800,
    [ValidateRange(1, 64)][int]$BuildParallelLevel = 6,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$WrapperArguments
)

# Runs a source root's tools/build-windows.ps1 under a bounded, windowless
# child console and records a JSON receipt beside its stdout/stderr.
#
# Do not replace this with Start-Process -RedirectStandardOutput: without
# -NoNewWindow, Windows PowerShell 5.1 creates the child with
# CREATE_NEW_CONSOLE, and on Windows 11 the default terminal hands that console
# to a visible, focusable Windows Terminal window. Closing it or pressing
# Ctrl+C there ends the whole build tree with 0xC000013A while the caller
# survives (docs/findings/native-wrapper-interruption-2026-10-19.md).
# CreateNoWindow gives the build its own console with no window, so neither a
# desktop close nor a Ctrl+C broadcast on the caller's console can reach it.

$ErrorActionPreference = 'Stop'

$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if ($SourceRoot.StartsWith('\\') -or -not [IO.Path]::IsPathRooted($SourceRoot)) {
    throw "SourceRoot must be a local drive path, not UNC: $SourceRoot"
}
$wrapper = Join-Path $SourceRoot 'tools\build-windows.ps1'
if (-not (Test-Path -LiteralPath $wrapper -PathType Leaf)) {
    throw "Build wrapper was not found: $wrapper"
}
# Evidence is never overwritten; a rerun needs a new directory.
New-Item -ItemType Directory -Path $EvidenceDirectory -ErrorAction Stop | Out-Null
$EvidenceDirectory = (Resolve-Path -LiteralPath $EvidenceDirectory).Path

function ConvertTo-CommandLineArgument([string]$Value) {
    if ($Value -and $Value -notmatch '[\s"]') { return $Value }
    return '"' + ($Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}
$arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $wrapper) + @($WrapperArguments)
$argumentLine = ($arguments | ForEach-Object { ConvertTo-CommandLineArgument $_ }) -join ' '
$powershell = Join-Path $PSHOME 'powershell.exe'

$started = Get-Date
$receipt = [ordered]@{
    source_root = $SourceRoot
    command = "powershell.exe $argumentLine"
    working_directory = $SourceRoot
    console = 'CreateNoWindow'
    started_at = $started.ToUniversalTime().ToString('o')
    timeout_seconds = $TimeoutSeconds
    build_parallel_level = "$BuildParallelLevel"
    status = 'running'
    exit_code = $null
}
$receiptPath = Join-Path $EvidenceDirectory 'build-receipt.json'
$receipt | ConvertTo-Json | Set-Content -LiteralPath $receiptPath

$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $powershell
$startInfo.Arguments = $argumentLine
$startInfo.WorkingDirectory = $SourceRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
# Do not touch ProcessStartInfo.Environment: on Windows PowerShell 5.1 its
# initialization is fragile when the inherited block has both Path and PATH.
# The child inherits this process's block, including the worker limit.
[Environment]::SetEnvironmentVariable('CMAKE_BUILD_PARALLEL_LEVEL',
    "$BuildParallelLevel", [EnvironmentVariableTarget]::Process)

$stdout = [IO.File]::Open((Join-Path $EvidenceDirectory 'build.stdout.txt'), 'CreateNew', 'Write', 'Read')
$stderr = [IO.File]::Open((Join-Path $EvidenceDirectory 'build.stderr.txt'), 'CreateNew', 'Write', 'Read')
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $startInfo
try {
    if (-not $process.Start()) { throw 'Native build process did not start.' }
    $receipt.process_id = $process.Id
    # Nothing may wait on interactive input (e.g. "Terminate batch job?").
    $process.StandardInput.Close()
    $stdoutCopy = $process.StandardOutput.BaseStream.CopyToAsync($stdout)
    $stderrCopy = $process.StandardError.BaseStream.CopyToAsync($stderr)
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        # Under 'Stop', Windows PowerShell 5.1 turns redirected native stderr
        # into a terminating error before the tree is recorded as killed.
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
        $receipt.status = if ($code -eq 0) { 'completed' } else { 'failed' }
        $receipt.exit_code = $code
    }
    # A grandchild that outlives a killed tree can hold the pipes open.
    $null = [Threading.Tasks.Task]::WaitAll(@($stdoutCopy, $stderrCopy), 30000)
} finally {
    $stdout.Dispose()
    $stderr.Dispose()
    $process.Dispose()
}
$receipt.elapsed_seconds = ((Get-Date) - $started).TotalSeconds
$receipt.finished_at = (Get-Date).ToUniversalTime().ToString('o')
$receipt | ConvertTo-Json | Set-Content -LiteralPath $receiptPath
$receipt | ConvertTo-Json
exit $code
