[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$temporaryDirectory = Join-Path $repositoryRoot 'MatterEditor\build\cmake\compiler-format-negative'
New-Item -ItemType Directory -Force -Path $temporaryDirectory | Out-Null
$bash = 'C:\msys64\usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) {
    throw "Pinned MSYS2 bash was not found: $bash"
}

function Convert-ToMsysPath([string]$path) {
    if ($path -notmatch '^([A-Za-z]):\\(.*)$') {
        throw "Expected a local Windows path, got: $path"
    }
    return '/' + $Matches[1].ToLowerInvariant() + '/' + $Matches[2].Replace('\', '/')
}

$msysRepositoryRoot = Convert-ToMsysPath $repositoryRoot
$msysTemporaryDirectory = Convert-ToMsysPath $temporaryDirectory
$command = "export PATH=/c/msys64/ucrt64/bin:/c/msys64/usr/bin:`$PATH; " +
    "export TMP='$msysTemporaryDirectory'; export TEMP='$msysTemporaryDirectory'; " +
    "cd '$msysRepositoryRoot'; " +
    'g++ -std=c++17 -Werror=format -IMatterEngine3/include ' +
    '-c MatterEngine3/tests/compiler_format_negative.cpp ' +
    "-o '$msysTemporaryDirectory/matter_compiler_format_negative.o'"

$ErrorActionPreference = 'Continue'
$diagnostic = (& $bash -lc $command 2>&1 | Out-String)
$compileResult = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($compileResult -eq 0) {
    throw "GNU accepted the intentionally invalid printf call; MATTER_PRINTF_FORMAT is inactive.`n$diagnostic"
}
if ($diagnostic -notmatch '\[-Werror=format(?:=)?\]') {
    throw "GNU failed for an unexpected reason instead of its format checker.`n$diagnostic"
}

Write-Host 'GNU MATTER_PRINTF_FORMAT negative compile rejected the invalid call as expected'
exit 0
