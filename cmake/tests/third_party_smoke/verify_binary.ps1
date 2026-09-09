[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,

    [Parameter(Mandatory = $true)]
    [string]$Ninja,

    [Parameter(Mandatory = $true)]
    [string]$AutoremesherLibrary,

    [string]$InputGraphFixture
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$executablePath = (Resolve-Path $Executable).Path
$buildPath = (Resolve-Path $BuildDirectory).Path
$ninjaPath = (Resolve-Path $Ninja).Path
$autoremesherLibraryPath = (Resolve-Path $AutoremesherLibrary).Path

Import-Module (Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1') -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot

$developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f `
    $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
$dumpCommand = '{0} && dumpbin.exe /dependents "{1}"' -f $developerEnvironment, $executablePath
$dumpOutput = (& $env:ComSpec /d /s /c $dumpCommand 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /dependents failed:`n$dumpOutput"
}

$forbiddenRuntimePattern = '(?i)libstdc\+\+|libgcc|libwinpthread'
if ($dumpOutput -match $forbiddenRuntimePattern) {
    throw "GNU runtime dependency found in smoke executable:`n$($Matches[0])"
}

$ninjaCommands = @(& $ninjaPath -C $buildPath -t commands matter_third_party_smoke 2>&1)
if ($LASTEXITCODE -ne 0 -or $ninjaCommands.Count -eq 0) {
    throw "Ninja command-closure query failed:`n$($ninjaCommands -join [Environment]::NewLine)"
}

$ninjaInputs = @(& $ninjaPath -C $buildPath -t inputs matter_third_party_smoke 2>&1)
if ($LASTEXITCODE -ne 0 -or $ninjaInputs.Count -eq 0) {
    throw "Ninja input-graph query failed or returned no inputs:`n$($ninjaInputs -join [Environment]::NewLine)"
}
if ($InputGraphFixture) {
    $ninjaInputs += $InputGraphFixture
}

$foreignInputPattern = '(?i)\.(?:a|o)$'
$foreignInputs = @($ninjaInputs | Where-Object { $_.Trim().Trim('"') -match $foreignInputPattern })
if ($foreignInputs) {
    throw "Non-MSVC input found in smoke input graph:`n$($foreignInputs -join [Environment]::NewLine)"
}

$toolchainContaminationPattern = '(?i)libstdc\+\+|libgcc|libwinpthread|mingw|msys|ucrt64'
$gnuToolPattern = '(?i)(?:^|[\s"])(?:[^\s"]*[\\/])?(?:gcc|g\+\+|ar)(?:\.exe)?(?=$|[\s"])'
$contaminatedCommands = $ninjaCommands | Where-Object {
    $_ -match $toolchainContaminationPattern -or $_ -match $gnuToolPattern
}
if ($contaminatedCommands) {
    throw "Non-MSVC input found in smoke command closure:`n$($contaminatedCommands -join [Environment]::NewLine)"
}

$foreignArtifacts = Get-ChildItem -LiteralPath $buildPath -Recurse -File | Where-Object {
    $_.Extension -in '.a', '.o' -or
    $_.Name -match '(?i)^(?:libstdc\+\+|libgcc|libwinpthread).*\.dll$'
}
if ($foreignArtifacts) {
    throw "GNU archive/object found in MSVC build tree:`n$($foreignArtifacts.FullName -join [Environment]::NewLine)"
}

$linkerMemberCommand = '{0} && dumpbin.exe /linkermember:2 "{1}"' -f `
    $developerEnvironment, $autoremesherLibraryPath
$linkerMembers = (& $env:ComSpec /d /s /c $linkerMemberCommand 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /linkermember failed:`n$linkerMembers"
}
$requiredRenamedSymbols = @(
    'matter_unused_real_nlInitExtension_SUPERLU',
    'matter_unused_real_nlExtensionIsInitialized_SUPERLU',
    'matter_unused_real_nlMatrixFactorize_SUPERLU',
    'matter_unused_real_nlInitExtension_CHOLMOD',
    'matter_unused_real_nlExtensionIsInitialized_CHOLMOD',
    'matter_unused_real_nlMatrixFactorize_CHOLMOD',
    'matter_unused_real_nlInitExtension_MKL',
    'matter_unused_real_nlExtensionIsInitialized_MKL',
    'matter_unused_real_NLMultMatrixVector_MKL',
    'matter_unused_real_nlMKLMatrixNewFromCRSMatrix',
    'matter_unused_real_nlMKLMatrixNewFromSparseMatrix',
    'matter_unused_real_nlInitExtension_CUDA',
    'matter_unused_real_nlExtensionIsInitialized_CUDA',
    'matter_unused_real_nlCUDABlas',
    'matter_unused_real_nlCUDAJacobiPreconditionerNewFromCRSMatrix',
    'matter_unused_real_nlCUDAMatrixNewFromCRSMatrix'
)
foreach ($symbol in $requiredRenamedSymbols) {
    if ($linkerMembers -notmatch "(?m)^\s+[0-9A-F]+\s+$([regex]::Escape($symbol))\s*$") {
        throw "Autoremesher archive is missing renamed optional-loader symbol: $symbol"
    }
}

$dependencies = [regex]::Matches($dumpOutput, '(?im)^\s+[^\s]+\.dll\s*$') |
    ForEach-Object { $_.Value.Trim() }
Write-Output "matter_third_party_binary_audit: clean"
Write-Output "  dependencies: $($dependencies -join ', ')"
Write-Output "  Ninja closure: $($ninjaCommands.Count) commands and $($ninjaInputs.Count) inputs; no .a/.o/MinGW/GNU runtime contamination"
Write-Output '  autoremesher: 16 optional-loader definitions renamed; fallback symbols authoritative'
