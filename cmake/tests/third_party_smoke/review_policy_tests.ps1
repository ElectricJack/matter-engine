[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$BuildDirectory,
    [Parameter(Mandatory = $true)] [string]$Ninja,
    [Parameter(Mandatory = $true)] [string]$AutoremesherLibrary,
    [Parameter(Mandatory = $true)] [string]$SmokeSource
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$buildPath = (Resolve-Path $BuildDirectory).Path
$ninjaPath = (Resolve-Path $Ninja).Path
$libraryPath = (Resolve-Path $AutoremesherLibrary).Path
$configHeader = Join-Path $repositoryRoot 'cmake\MatterAutoremesherConfig.h'

if (-not (Test-Path -LiteralPath $configHeader)) {
    throw "Autoremesher forced-include config header is missing: $configHeader"
}
$headerText = Get-Content -Raw $configHeader
if ($headerText -notmatch '(?m)^#\s*undef\s+NDEBUG\s*$') {
    throw 'Autoremesher config header must undefine NDEBUG'
}

$commands = @(& $ninjaPath -C $buildPath -t commands matter_autoremesher 2>&1)
if ($LASTEXITCODE -ne 0 -or $commands.Count -eq 0) {
    throw 'Ninja returned no autoremesher command closure'
}
$compileCommands = @($commands | Where-Object { $_ -match '\bcl\.exe\b' })
if ($compileCommands.Count -ne 172) {
    throw "Expected 172 autoremesher compile commands, got $($compileCommands.Count)"
}
$normalizedConfigHeader = $configHeader.Replace('\', '/')
$withoutForcedHeader = @($compileCommands | Where-Object {
    -not $_.Contains("/FI$normalizedConfigHeader") -and
    -not $_.Contains("-FI$normalizedConfigHeader") -and
    -not $_.Contains("/FI`"$normalizedConfigHeader`"") -and
    -not $_.Contains("-FI`"$normalizedConfigHeader`"")
})
if ($withoutForcedHeader.Count -ne 0) {
    throw "$($withoutForcedHeader.Count) autoremesher commands do not force-include the NDEBUG config"
}
if ($compileCommands | Where-Object { $_ -match '/UNDEBUG' }) {
    throw 'Autoremesher must not use /UNDEBUG because it conflicts with /DNDEBUG'
}

$renamePolicies = [ordered]@{
    'nl_superlu.c' = @(
        'nlInitExtension_SUPERLU=matter_unused_real_nlInitExtension_SUPERLU',
        'nlExtensionIsInitialized_SUPERLU=matter_unused_real_nlExtensionIsInitialized_SUPERLU',
        'nlMatrixFactorize_SUPERLU=matter_unused_real_nlMatrixFactorize_SUPERLU'
    )
    'nl_cholmod.c' = @(
        'nlInitExtension_CHOLMOD=matter_unused_real_nlInitExtension_CHOLMOD',
        'nlExtensionIsInitialized_CHOLMOD=matter_unused_real_nlExtensionIsInitialized_CHOLMOD',
        'nlMatrixFactorize_CHOLMOD=matter_unused_real_nlMatrixFactorize_CHOLMOD'
    )
    'nl_mkl.c' = @(
        'nlInitExtension_MKL=matter_unused_real_nlInitExtension_MKL',
        'nlExtensionIsInitialized_MKL=matter_unused_real_nlExtensionIsInitialized_MKL',
        'NLMultMatrixVector_MKL=matter_unused_real_NLMultMatrixVector_MKL',
        'nlMKLMatrixNewFromCRSMatrix=matter_unused_real_nlMKLMatrixNewFromCRSMatrix',
        'nlMKLMatrixNewFromSparseMatrix=matter_unused_real_nlMKLMatrixNewFromSparseMatrix'
    )
    'nl_cuda.c' = @(
        'nlInitExtension_CUDA=matter_unused_real_nlInitExtension_CUDA',
        'nlExtensionIsInitialized_CUDA=matter_unused_real_nlExtensionIsInitialized_CUDA',
        'nlCUDABlas=matter_unused_real_nlCUDABlas',
        'nlCUDAJacobiPreconditionerNewFromCRSMatrix=matter_unused_real_nlCUDAJacobiPreconditionerNewFromCRSMatrix',
        'nlCUDAMatrixNewFromCRSMatrix=matter_unused_real_nlCUDAMatrixNewFromCRSMatrix'
    )
}
foreach ($entry in $renamePolicies.GetEnumerator()) {
    $sourcePattern = '[\\/]{0}(?:"|\s|$)' -f ([regex]::Escape($entry.Key))
    $sourceCommand = @($compileCommands | Where-Object { $_ -match $sourcePattern })
    if ($sourceCommand.Count -ne 1) {
        throw "Expected one compile command for $($entry.Key), got $($sourceCommand.Count)"
    }
    foreach ($definition in $entry.Value) {
        if (-not $sourceCommand[0].Contains($definition)) {
            throw "$($entry.Key) does not rename $definition"
        }
    }
}

$smokeText = Get-Content -Raw (Resolve-Path $SmokeSource)
if ($smokeText -notmatch 'autoremesher::remesh\s*\(' -or
    $smokeText -notmatch 'empty input') {
    throw 'Smoke must call autoremesher::remesh and validate its empty-input failure'
}

Import-Module (Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1') -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot
$developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f `
    $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
$dumpCommand = '{0} && dumpbin.exe /linkermember:2 "{1}"' -f $developerEnvironment, $libraryPath
$symbols = (& $env:ComSpec /d /s /c $dumpCommand 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /linkermember failed:`n$symbols"
}

$renamedSymbols = @(
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
$fallbackSymbols = $renamedSymbols | ForEach-Object { $_ -replace '^matter_unused_real_', '' }
foreach ($symbol in $fallbackSymbols) {
    if ($symbols -notmatch "(?m)^\s+[0-9A-F]+\s+$([regex]::Escape($symbol))\s*$") {
        throw "Autoremesher archive is missing fallback public symbol: $symbol"
    }
}
foreach ($symbol in $renamedSymbols) {
    if ($symbols -notmatch "(?m)^\s+[0-9A-F]+\s+$([regex]::Escape($symbol))\s*$") {
        throw "Autoremesher archive is missing renamed optional-loader symbol: $symbol"
    }
}

Write-Output 'matter_third_party_review_policy: clean'
Write-Output '  autoremesher: forced NDEBUG undef on 172 commands'
Write-Output '  NL optional loaders: 16 real definitions renamed; fallback names authoritative'
Write-Output '  smoke: empty-input remesh call present'
