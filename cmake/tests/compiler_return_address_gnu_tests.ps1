[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$temporaryDirectory = Join-Path $repositoryRoot 'MatterEditor\build\cmake\compiler-return-address-gnu'
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
    'g++ -std=c++17 -O0 -IMatterEngine3/include ' +
    'MatterEngine3/tests/compiler_return_address_gnu_tests.cpp ' +
    "-o '$msysTemporaryDirectory/compiler_return_address_gnu_tests.exe'; " +
    "'$msysTemporaryDirectory/compiler_return_address_gnu_tests.exe'"

$ErrorActionPreference = 'Continue'
$diagnostic = (& $bash -lc $command 2>&1 | Out-String)
$testResult = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($testResult -ne 0) {
    throw "GNU return-address caller-identity test failed.`n$diagnostic"
}

Write-Host 'GNU return-address caller identity preserved at -O0'
exit 0
