[CmdletBinding()]
param(
    [string]$RepositoryRoot,
    [string]$MsvcEditor,
    [string]$OutputDirectory,
    [switch]$ValidatorOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $RepositoryRoot) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
if (-not $MsvcEditor) {
    $MsvcEditor = Join-Path $RepositoryRoot 'MatterEditor\build\windows-msvc\editor.exe'
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $RepositoryRoot `
        'MatterEditor\build\baselines\msvc\registration-census-contract'
}

# The regression is the real default runner, not a source-text assertion. The
# unavailable rollback editor must neither be required nor launched. Before the
# native-only repair this fails at the old unconditional MinGW existence check,
# without starting either editor.
if (-not $ValidatorOnly) {
    $unavailableMinGW = Join-Path $OutputDirectory 'rollback-editor-must-not-be-required.exe'
    if (Test-Path -LiteralPath $unavailableMinGW) {
        throw "native-only regression requires a nonexistent rollback path: $unavailableMinGW"
    }
    & (Join-Path $PSScriptRoot 'editor_registration_census.ps1') `
        -RepositoryRoot $RepositoryRoot -MsvcEditor $MsvcEditor `
        -MinGWEditor $unavailableMinGW -OutputDirectory $OutputDirectory
    Write-Host 'default registration census succeeds without a MinGW editor'
}

. (Join-Path $PSScriptRoot 'editor_registration_contract.ps1')

function Assert-Rejected([scriptblock]$Action, [string]$Diagnostic) {
    $rejected = $false
    try {
        & $Action
    } catch {
        if (-not $_.Exception.Message.Contains($Diagnostic)) { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw "census accepted invalid fixture: $Diagnostic" }
}

# Literal fixtures are independent of both the production manifest and parser.
# Losing a registration, silently accepting a new one, or folding JS identifier
# case must fail for every registry, even when the remaining names still match.
$expected = [ordered]@{
    world = @('Alpha', 'Beta')
    dsl = @('__dsl_box', '__dsl_rayTraced')
    property = @('render.lighting', 'viewer.session')
    editor = @('fifo.character', 'viewer.reload')
}
$validJson = '{"world":["Beta","Alpha"],"dsl":["__dsl_rayTraced","__dsl_box"],"property":["viewer.session","render.lighting"],"editor":["viewer.reload","fifo.character"]}'
$prefix = 'MATTER_REGISTRATION_CENSUS_JSON='
$valid = ConvertFrom-RuntimeRegistrationCensusLog `
    "ordinary startup log`n$prefix$validJson`n" 'fixture'
Assert-RegistrationCensusMatches $expected $valid 'fixture'

foreach ($category in @('world', 'dsl', 'property', 'editor')) {
    $missing = $validJson | ConvertFrom-Json
    $missing.$category = @($missing.$category[0])
    Assert-Rejected {
        $parsed = ConvertFrom-RegistrationCensusRecord $missing 'fixture'
        Assert-RegistrationCensusMatches $expected $parsed 'fixture'
    } "missing from fixture"

    $extra = $validJson | ConvertFrom-Json
    $extra.$category += '__unexpected_live_registration__'
    Assert-Rejected {
        $parsed = ConvertFrom-RegistrationCensusRecord $extra 'fixture'
        Assert-RegistrationCensusMatches $expected $parsed 'fixture'
    } 'extra in fixture'

    $duplicate = $validJson | ConvertFrom-Json
    $duplicate.$category += $duplicate.$category[0]
    Assert-Rejected {
        ConvertFrom-RegistrationCensusRecord $duplicate 'fixture'
    } "duplicate '$category'"

    $wrongCase = $validJson | ConvertFrom-Json
    $wrongCase.$category[0] = $wrongCase.$category[0].ToUpperInvariant()
    Assert-Rejected {
        $parsed = ConvertFrom-RegistrationCensusRecord $wrongCase 'fixture'
        Assert-RegistrationCensusMatches $expected $parsed 'fixture'
    } "$category registration mismatch"

    $scalar = $validJson | ConvertFrom-Json
    $scalar.$category = $scalar.$category[0]
    Assert-Rejected {
        ConvertFrom-RegistrationCensusRecord $scalar 'fixture'
    } "nonempty array for '$category'"

    $nonString = $validJson | ConvertFrom-Json
    $nonString.$category[0] = 42
    Assert-Rejected {
        ConvertFrom-RegistrationCensusRecord $nonString 'fixture'
    } "invalid '$category' registration"
}

$missingCategory = $validJson | ConvertFrom-Json
$missingCategory.PSObject.Properties.Remove('world')
Assert-Rejected {
    ConvertFrom-RegistrationCensusRecord $missingCategory 'fixture'
} "omitted 'world'"
$unknownCategory = $validJson | ConvertFrom-Json
$unknownCategory | Add-Member -NotePropertyName 'unknown' -NotePropertyValue @('value')
Assert-Rejected {
    ConvertFrom-RegistrationCensusRecord $unknownCategory 'fixture'
} "unexpected category 'unknown'"
Assert-Rejected {
    ConvertFrom-RuntimeRegistrationCensusLog 'ordinary log only' 'fixture'
} 'expected exactly one'
Assert-Rejected {
    ConvertFrom-RuntimeRegistrationCensusLog "$prefix$validJson`n$prefix$validJson" 'fixture'
} 'expected exactly one'
Assert-Rejected {
    ConvertFrom-RuntimeRegistrationCensusLog "$prefix{" 'fixture'
} 'invalid runtime census JSON'

Write-Host 'registration census contract tests passed: exact sets, ordinal names, schema, and live-record framing'
