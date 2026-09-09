[CmdletBinding()]
param(
    [string]$RepositoryRoot,
    [string]$MinGWEditor,
    [string]$MsvcEditor,
    [string]$OutputDirectory,
    [switch]$CompareMinGW
)

$ErrorActionPreference = 'Stop'

if (-not $RepositoryRoot) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
if ($CompareMinGW -and -not $MinGWEditor) {
    $MinGWEditor = Join-Path $RepositoryRoot 'MatterEditor\build\windows\editor.exe'
}
if (-not $MsvcEditor) {
    $MsvcEditor = Join-Path $RepositoryRoot 'MatterEditor\build\windows-msvc\editor.exe'
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $RepositoryRoot `
        'MatterEditor\build\baselines\msvc\registration-census'
}

$editorsToValidate = @($MsvcEditor)
if ($CompareMinGW) { $editorsToValidate += $MinGWEditor }
foreach ($editor in $editorsToValidate) {
    if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) {
        throw "$editor was not found"
    }
}

. (Join-Path $PSScriptRoot 'editor_registration_contract.ps1')
$expectedPath = Join-Path $PSScriptRoot 'editor_registration_expected.json'
$expectedRecord = Get-Content -LiteralPath $expectedPath -Raw | ConvertFrom-Json
$expected = ConvertFrom-RegistrationCensusRecord $expectedRecord 'reviewed manifest'

function Invoke-RuntimeCensus([string]$Path, [string]$LogPath) {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Path
    $start.WorkingDirectory = Join-Path $RepositoryRoot 'MatterEditor'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    [void]$start.EnvironmentVariables.Remove('MATTER_WORLD')
    $start.EnvironmentVariables['MATTER_REGISTRATION_CENSUS'] = '1'
    $start.EnvironmentVariables['MATTER_HIDE_UI'] = '1'
    $start.EnvironmentVariables['TMP'] = [System.IO.Path]::GetTempPath()
    $start.EnvironmentVariables['TEMP'] = [System.IO.Path]::GetTempPath()

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    [void]$process.Start()
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) {
        $process.Kill()
        throw "$Path did not finish its runtime registration census within 30 seconds"
    }
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    ($stdout + "`n" + $stderr) |
        Set-Content -LiteralPath $LogPath -Encoding UTF8
    if ($process.ExitCode -ne 0) {
        throw "$Path runtime registration census failed with exit $($process.ExitCode)`n$stdout`n$stderr"
    }

    return ConvertFrom-RuntimeRegistrationCensusLog $stdout $Path
}

[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$msvcLog = Join-Path $OutputDirectory 'msvc-runtime.log'
$msvc = Invoke-RuntimeCensus $MsvcEditor $msvcLog
Assert-RegistrationCensusMatches $expected $msvc 'MSVC editor.exe'

# Rollback parity is never part of normal CTest. It requires this explicit
# opt-in and compares against the same reviewed contract, not a stale oracle.
if ($CompareMinGW) {
    $mingw = Invoke-RuntimeCensus $MinGWEditor `
        (Join-Path $OutputDirectory 'mingw-runtime.log')
    Assert-RegistrationCensusMatches $expected $mingw 'MinGW editor.exe'
}

# A linked byte string is not a registration.  Appending a PE overlay proves the
# diagnostic observes live registries rather than treating strings(1) output as
# authority: Windows loads the copied editor, while its bytes contain the decoy.
$decoy = '__dsl_unregistered_binary_decoy__'
$decoyEditor = Join-Path $OutputDirectory 'editor-unregistered-decoy.exe'
[System.IO.File]::Copy($MsvcEditor, $decoyEditor, $true)
$stream = [System.IO.File]::Open($decoyEditor,
    [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write,
    [System.IO.FileShare]::Read)
try {
    $decoyBytes = [System.Text.Encoding]::ASCII.GetBytes($decoy + [char]0)
    $stream.Write($decoyBytes, 0, $decoyBytes.Length)
} finally {
    $stream.Dispose()
}
$mutatedBytes = [System.Text.Encoding]::GetEncoding(28591).GetString(
    [System.IO.File]::ReadAllBytes($decoyEditor))
if (-not $mutatedBytes.Contains($decoy)) {
    throw 'negative census fixture does not contain its unregistered decoy string'
}
$decoyCensus = Invoke-RuntimeCensus $decoyEditor `
    (Join-Path $OutputDirectory 'msvc-unregistered-decoy-runtime.log')
foreach ($category in @('world', 'dsl', 'property', 'editor')) {
    if ($decoy -cin $decoyCensus[$category]) {
        throw "unregistered binary decoy was falsely reported as a $category registration"
    }
}
Assert-RegistrationCensusMatches $expected $decoyCensus `
    'MSVC editor.exe with unregistered string overlay'

if ($CompareMinGW) {
    $mingwRecord = [ordered]@{
        compiler = 'MinGW'
        executable = (Resolve-Path -LiteralPath $MinGWEditor).Path
        registrations = $mingw
    }
    $mingwRecord | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath (Join-Path $OutputDirectory 'mingw.json') -Encoding UTF8
}
# CTest may inherit a restricted PowerShell module path. Keep provenance
# independent of Get-FileHash/utility-module discovery in that environment.
$manifestHasher = [System.Security.Cryptography.SHA256]::Create()
try {
    $manifestStream = [System.IO.File]::OpenRead($expectedPath)
    try {
        $manifestSha256 = [System.BitConverter]::ToString(
            $manifestHasher.ComputeHash($manifestStream)).Replace('-', '')
    } finally {
        $manifestStream.Dispose()
    }
} finally {
    $manifestHasher.Dispose()
}
$msvcRecord = [ordered]@{
    compiler = 'MSVC'
    executable = (Resolve-Path -LiteralPath $MsvcEditor).Path
    expected_manifest = $expectedPath
    expected_manifest_sha256 = $manifestSha256
    registrations = $msvc
}
$msvcRecord | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'msvc.json') -Encoding UTF8

Write-Host "runtime registration census passed: worlds=$($msvc.world.Count) dsl=$($msvc.dsl.Count) property=$($msvc.property.Count) editor=$($msvc.editor.Count); unregistered binary decoy rejected"
