[CmdletBinding()]
param(
    [string]$RepositoryRoot,
    [string]$MinGWEditor,
    [string]$MsvcEditor,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

if (-not $RepositoryRoot) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
if (-not $MinGWEditor) {
    $MinGWEditor = Join-Path $RepositoryRoot 'MatterEditor\build\windows\editor.exe'
}
if (-not $MsvcEditor) {
    $MsvcEditor = Join-Path $RepositoryRoot 'MatterEditor\build\windows-msvc\editor.exe'
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $RepositoryRoot `
        'MatterEditor\build\baselines\msvc\registration-census'
}

foreach ($editor in @($MinGWEditor, $MsvcEditor)) {
    if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) {
        throw "$editor was not found"
    }
}

function Get-SortedUnique([System.Collections.IEnumerable]$Items) {
    return @($Items | Where-Object { $_ } | Sort-Object -Unique)
}

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

    $prefix = 'MATTER_REGISTRATION_CENSUS_JSON='
    $records = @($stdout -split "`r?`n" |
        Where-Object { $_.StartsWith($prefix, [System.StringComparison]::Ordinal) })
    if ($records.Count -ne 1) {
        throw "$Path emitted $($records.Count) runtime census records; expected exactly one`n$stdout`n$stderr"
    }
    try {
        $record = $records[0].Substring($prefix.Length) | ConvertFrom-Json
    } catch {
        throw "$Path emitted invalid runtime census JSON: $($records[0])"
    }

    $result = [ordered]@{}
    foreach ($category in @('world', 'dsl', 'property', 'editor')) {
        $property = $record.PSObject.Properties[$category]
        if ($null -eq $property) {
            throw "$Path runtime census omitted '$category'"
        }
        $values = @($property.Value | ForEach-Object { [string]$_ })
        $unique = Get-SortedUnique $values
        if ($values.Count -eq 0) {
            throw "$Path runtime census reported no '$category' registrations"
        }
        if ($values.Count -ne $unique.Count) {
            throw "$Path runtime census reported duplicate '$category' registrations"
        }
        $result[$category] = $unique
    }
    return $result
}

function Compare-Set([string]$Category, [string]$LeftName, $Left,
                     [string]$RightName, $Right) {
    $missing = @($Left | Where-Object { $_ -notin $Right })
    $extra = @($Right | Where-Object { $_ -notin $Left })
    if ($missing.Count -eq 0 -and $extra.Count -eq 0) { return }

    $message = "$Category registration mismatch: $LeftName vs $RightName"
    if ($missing.Count -gt 0) {
        $message += "`n  missing from ${RightName}: $($missing -join ', ')"
    }
    if ($extra.Count -gt 0) {
        $message += "`n  extra in ${RightName}: $($extra -join ', ')"
    }
    throw $message
}

[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$mingwLog = Join-Path $OutputDirectory 'mingw-runtime.log'
$msvcLog = Join-Path $OutputDirectory 'msvc-runtime.log'
$mingw = Invoke-RuntimeCensus $MinGWEditor $mingwLog
$msvc = Invoke-RuntimeCensus $MsvcEditor $msvcLog

foreach ($category in @('world', 'dsl', 'property', 'editor')) {
    Compare-Set $category 'MinGW editor.exe' $mingw[$category] `
        'MSVC editor.exe' $msvc[$category]
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
    if ($decoy -in $decoyCensus[$category]) {
        throw "unregistered binary decoy was falsely reported as a $category registration"
    }
    Compare-Set $category 'MSVC editor.exe' $msvc[$category] `
        'MSVC editor.exe with unregistered string overlay' $decoyCensus[$category]
}

$mingwRecord = [ordered]@{
    compiler = 'MinGW'
    executable = (Resolve-Path -LiteralPath $MinGWEditor).Path
    registrations = $mingw
}
$msvcRecord = [ordered]@{
    compiler = 'MSVC'
    executable = (Resolve-Path -LiteralPath $MsvcEditor).Path
    registrations = $msvc
}
$mingwRecord | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'mingw.json') -Encoding UTF8
$msvcRecord | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'msvc.json') -Encoding UTF8

Write-Host "runtime registration census passed: worlds=$($msvc.world.Count) dsl=$($msvc.dsl.Count) property=$($msvc.property.Count) editor=$($msvc.editor.Count); unregistered binary decoy rejected"
