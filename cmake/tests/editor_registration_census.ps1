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
    $OutputDirectory = Join-Path $RepositoryRoot 'MatterEditor\build\baselines\msvc\registration-census'
}

foreach ($editor in @($MinGWEditor, $MsvcEditor)) {
    if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) {
        throw "$editor was not found"
    }
}

function Get-SourceText([string[]]$paths) {
    $builder = [System.Text.StringBuilder]::new()
    foreach ($path in $paths) {
        [void]$builder.AppendLine([System.IO.File]::ReadAllText($path))
    }
    return $builder.ToString()
}

function Get-SortedUnique([System.Collections.IEnumerable]$items) {
    return @($items | Where-Object { $_ } | Sort-Object -Unique)
}

function Remove-CppComments([string]$text) {
    $builder = [System.Text.StringBuilder]::new($text.Length)
    $inLineComment = $false
    $inBlockComment = $false
    $quote = [char]0
    $escaped = $false
    for ($index = 0; $index -lt $text.Length; ++$index) {
        $character = $text[$index]
        $next = if ($index + 1 -lt $text.Length) { $text[$index + 1] } else { [char]0 }

        if ($inLineComment) {
            if ($character -eq "`n") {
                $inLineComment = $false
                [void]$builder.Append($character)
            } else {
                [void]$builder.Append(' ')
            }
            continue
        }
        if ($inBlockComment) {
            if ($character -eq '*' -and $next -eq '/') {
                [void]$builder.Append(' ')
                [void]$builder.Append(' ')
                ++$index
                $inBlockComment = $false
            } elseif ($character -eq "`n") {
                [void]$builder.Append($character)
            } else {
                [void]$builder.Append(' ')
            }
            continue
        }
        if ($quote -ne [char]0) {
            [void]$builder.Append($character)
            if ($escaped) {
                $escaped = $false
            } elseif ($character -eq '\') {
                $escaped = $true
            } elseif ($character -eq $quote) {
                $quote = [char]0
            }
            continue
        }
        if ($character -eq '/' -and $next -eq '/') {
            [void]$builder.Append(' ')
            [void]$builder.Append(' ')
            ++$index
            $inLineComment = $true
        } elseif ($character -eq '/' -and $next -eq '*') {
            [void]$builder.Append(' ')
            [void]$builder.Append(' ')
            ++$index
            $inBlockComment = $true
        } else {
            [void]$builder.Append($character)
            if ($character -eq '"' -or $character -eq "'") {
                $quote = $character
            }
        }
    }
    return $builder.ToString()
}

function Get-AsciiStringSet([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $text = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
    $set = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($match in [regex]::Matches($text, '[\x20-\x7e]{4,}')) {
        [void]$set.Add($match.Value)
    }
    return $set
}

function Get-ExpectedRegistrations {
    $dslPaths = @(
        (Join-Path $RepositoryRoot 'MatterEngine3\src\dsl_bindings.cpp'),
        (Join-Path $RepositoryRoot 'MatterEngine3\src\pf_bindings.cpp')
    )
    $dslText = Remove-CppComments (Get-SourceText $dslPaths)
    $dsl = foreach ($match in [regex]::Matches(
            $dslText, '\bbind\s*\(\s*"([^"]+)"')) {
        $match.Groups[1].Value
    }

    $propertyPaths = @(
        Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot 'MatterEditor\src') `
            -Recurse -File -Include '*.cpp', '*.h'
        Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot 'MatterEngine3\src') `
            -Recurse -File -Include '*.cpp', '*.h'
        Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot 'MatterEngine3\include') `
            -Recurse -File -Include '*.cpp', '*.h'
    ) | ForEach-Object FullName
    $propertyText = Remove-CppComments (Get-SourceText $propertyPaths)
    $properties = foreach ($match in [regex]::Matches(
            $propertyText,
            'props::group\s*<[^>]+>\s*\(\s*"([^"]+)"',
            [System.Text.RegularExpressions.RegexOptions]::Singleline)) {
        $match.Groups[1].Value
    }

    $editorPaths = @(
        Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot 'MatterEditor\src') `
            -Recurse -File -Include '*.cpp', '*.h'
        Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot 'MatterEngine3\src') `
            -Recurse -File -Include '*.cpp', '*.h'
        Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot 'MatterEngine3\include') `
            -Recurse -File -Include '*.cpp', '*.h'
    ) | ForEach-Object FullName
    $editorText = Remove-CppComments (Get-SourceText $editorPaths)
    $editor = foreach ($match in [regex]::Matches(
            $editorText, 'MT_COMMAND_NAME\s*\(\s*"([^"]+)"')) {
        $match.Groups[1].Value
    }

    return [ordered]@{
        dsl = Get-SortedUnique $dsl
        property = Get-SortedUnique $properties
        editor = Get-SortedUnique $editor
    }
}

function Get-BinaryRegistrations([string]$path, $expected) {
    $strings = Get-AsciiStringSet $path
    $result = [ordered]@{}
    foreach ($category in @('dsl', 'property', 'editor')) {
        $present = foreach ($name in $expected[$category]) {
            if ($strings.Contains($name)) { $name }
        }
        $result[$category] = Get-SortedUnique $present
    }
    return $result
}

function Invoke-WorldDiagnostic([string]$path) {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $path
    $start.WorkingDirectory = Join-Path $RepositoryRoot 'MatterEditor'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['MATTER_WORLD'] = '__matter_registration_census_invalid__'
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
        throw "$path did not finish its list-worlds diagnostic within 30 seconds"
    }
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    $log = $stdout + "`n" + $stderr
    if ($process.ExitCode -eq 0 -or
            $log -notmatch "MATTER_WORLD '__matter_registration_census_invalid__' is not a committed world") {
        throw "$path did not complete the expected list-worlds diagnostic (exit $($process.ExitCode))`n$log"
    }

    $worlds = foreach ($line in ($stdout -split "`r?`n")) {
        if ($line -match '^\s+\[\d+\]\s+(.+?)\s+\(.+\)\s*$') {
            $Matches[1]
        }
    }
    $worlds = Get-SortedUnique $worlds
    if ($worlds.Count -eq 0) {
        throw "$path list-worlds diagnostic reported no worlds`n$log"
    }
    return [ordered]@{ worlds = $worlds; log = $log }
}

function Compare-Set([string]$category, [string]$leftName, $left,
                     [string]$rightName, $right) {
    $missing = @($left | Where-Object { $_ -notin $right })
    $extra = @($right | Where-Object { $_ -notin $left })
    if ($missing.Count -eq 0 -and $extra.Count -eq 0) { return }

    $message = "$category registration mismatch: $leftName vs $rightName"
    if ($missing.Count -gt 0) {
        $message += "`n  missing from ${rightName}: $($missing -join ', ')"
    }
    if ($extra.Count -gt 0) {
        $message += "`n  extra in ${rightName}: $($extra -join ', ')"
    }
    throw $message
}

$expected = Get-ExpectedRegistrations
$mingw = Get-BinaryRegistrations $MinGWEditor $expected
$msvc = Get-BinaryRegistrations $MsvcEditor $expected
$mingwWorlds = Invoke-WorldDiagnostic $MinGWEditor
$msvcWorlds = Invoke-WorldDiagnostic $MsvcEditor
$mingw['world'] = $mingwWorlds.worlds
$msvc['world'] = $msvcWorlds.worlds

foreach ($category in @('dsl', 'property', 'editor')) {
    Compare-Set $category 'source registration inventory' $expected[$category] `
        'MinGW editor.exe' $mingw[$category]
    Compare-Set $category 'source registration inventory' $expected[$category] `
        'MSVC editor.exe' $msvc[$category]
    Compare-Set $category 'MinGW editor.exe' $mingw[$category] `
        'MSVC editor.exe' $msvc[$category]
}
Compare-Set 'world' 'MinGW editor.exe' $mingw.world 'MSVC editor.exe' $msvc.world

[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
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
$mingwWorlds.log | Set-Content -LiteralPath (Join-Path $OutputDirectory 'mingw-worlds.log') -Encoding UTF8
$msvcWorlds.log | Set-Content -LiteralPath (Join-Path $OutputDirectory 'msvc-worlds.log') -Encoding UTF8

Write-Host "registration census passed: worlds=$($msvc.world.Count) dsl=$($msvc.dsl.Count) property=$($msvc.property.Count) editor=$($msvc.editor.Count)"
