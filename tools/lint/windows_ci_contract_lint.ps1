$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Import-Module (Join-Path $repository 'tools/windows/MatterWindowsCi.psm1') -Force
function Assert([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Assert-Rejects([scriptblock]$Action, [string]$Pattern) {
    try { & $Action; throw 'Expected rejection was absent.' }
    catch { if ($_.Exception.Message -notmatch $Pattern) { throw } }
}
Assert ((Get-MatterCiTier -Ref 'refs/heads/dev') -eq 'full') 'dev must retain the full required check'
Assert ((Get-MatterCiTier -Ref 'refs/heads/aq/batches/test') -eq 'full') 'batch v1 qualification must use full'
Assert ((Get-MatterCiTier -Ref 'refs/heads/aq/integration/test') -eq 'full') 'integration v1 qualification must use full'
Assert ((Get-MatterCiTier -Ref 'refs/pull/17/merge' -BaseRef dev) -eq 'full') 'PR v1 qualification must use full'
Assert ((Get-MatterCiTier -Ref 'refs/heads/staging') -eq 'full') 'staging must use full'
Assert ((Get-MatterCiTier -Ref 'refs/heads/main') -eq 'full') 'main must use full'
Assert ((Get-MatterCiTier -Ref 'refs/heads/dev' -EventName schedule) -eq 'full') 'nightly must use full'
Assert-Rejects { Get-MatterCiTier -Ref 'refs/heads/main' -Tier fast } 'require the full tier'
Assert ((Get-MatterCiTier -Ref 'refs/pull/17/merge' -BaseRef main) -eq 'full') 'PR to main must use full'
Assert ((Get-MatterCiTier -Ref 'refs/pull/17/merge' -BaseRef staging) -eq 'full') 'PR to staging must use full'
Assert-Rejects { Get-MatterCiTier -Ref 'refs/pull/17/merge' -BaseRef staging -Tier fast } 'require the full tier'
Assert-Rejects { Get-MatterCiTier -Ref 'refs/heads/dev' -Tier auto -RequireExplicitTier } 'explicit fast or full tier'
Assert (-not (Get-MatterCiPackageRequired -Ref 'refs/pull/17/merge' -EventName pull_request -Tier full)) 'PR must not package'
Assert (-not (Get-MatterCiPackageRequired -Ref 'refs/heads/dev' -EventName push -Tier full)) 'dev must not package'
Assert (Get-MatterCiPackageRequired -Ref 'refs/heads/staging' -EventName push -Tier full) 'staging promotion must package'
Assert (Get-MatterCiPackageRequired -Ref 'refs/heads/main' -EventName push -Tier full) 'main promotion must package'
Assert (Get-MatterCiPackageRequired -EventName schedule -Tier full) 'nightly must package'
Assert-Rejects { Get-MatterCiPackageRequired -Tier fast -Package } 'requires the full tier'

$fixture = Join-Path ([IO.Path]::GetTempPath()) ('matter-ci-contract-' + [Guid]::NewGuid().ToString('N'))
$ciRoot = Join-Path $fixture 'ci'
$source = Join-Path $fixture 'source'
New-Item -ItemType Directory -Path $source -Force | Out-Null
try {
    # Hash actual source/tool inputs, rather than only passing synthetic keys to
    # the state machine. Ordinary source edits must retain incremental objects.
    New-Item -ItemType Directory -Force -Path (Join-Path $source 'cmake'),
        (Join-Path $source 'tools/windows'), (Join-Path $source 'vs/VC/Tools/MSVC/fixture/bin/Hostx64/x64') | Out-Null
    foreach ($name in @('CMakeLists.txt', 'CMakePresets.json', 'tools/build-windows.ps1',
            'cmake/fixture.cmake', 'tools/windows/toolchain.lock.json',
            'cmake.exe', 'ninja.exe', 'glslc.exe', 'vs/VC/Tools/MSVC/fixture/bin/Hostx64/x64/cl.exe')) {
        Set-Content -LiteralPath (Join-Path $source $name) -Value 'initial'
    }
    $tools = [pscustomobject]@{ CMake = (Join-Path $source 'cmake.exe'); Ninja = (Join-Path $source 'ninja.exe');
        Glslc = (Join-Path $source 'glslc.exe'); VisualStudioRoot = (Join-Path $source 'vs'); MsvcToolsVersion = 'fixture' }
    $before = Get-MatterCiFingerprint -RepositoryRoot $source -Toolchain $tools
    Set-Content -LiteralPath (Join-Path $source 'ordinary.cpp') -Value 'new source'
    Assert ((Get-MatterCiFingerprint -RepositoryRoot $source -Toolchain $tools).Key -eq $before.Key) 'source edit defeated incremental build'
    Set-Content -LiteralPath (Join-Path $source 'cmake/fixture.cmake') -Value 'new build definition'
    $after = Get-MatterCiFingerprint -RepositoryRoot $source -Toolchain $tools
    Assert ($after.Key -ne $before.Key) 'CMake edit did not invalidate build'
    Set-Content -LiteralPath $tools.Glslc -Value 'upgraded compiler'
    Assert ((Get-MatterCiFingerprint -RepositoryRoot $source -Toolchain $tools).Key -ne $after.Key) 'in-place tool upgrade did not invalidate build'
    Copy-Item (Join-Path $repository 'tools/ci-windows.ps1') (Join-Path $source 'tools/ci-windows.ps1')
    Copy-Item (Join-Path $repository 'tools/windows/MatterWindowsCi.psm1') (Join-Path $source 'tools/windows/MatterWindowsCi.psm1')
    $oldEvidence = Join-Path $source 'artifacts/ci'
    New-Item -ItemType Directory -Force -Path $oldEvidence | Out-Null
    Set-Content (Join-Path $oldEvidence 'stale.fixture') 'previous run'
    $savedRef = $env:GITHUB_REF
    $savedErrorAction = $ErrorActionPreference
    try {
        $env:GITHUB_REF = 'refs/heads/staging'
        $ErrorActionPreference = 'Continue'
        $invalidOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File (Join-Path $source 'tools/ci-windows.ps1') -Tier fast 2>&1 | Out-String
        $invalidExit = $LASTEXITCODE
    } finally {
        $env:GITHUB_REF = $savedRef
        $ErrorActionPreference = $savedErrorAction
    }
    Assert ($invalidExit -ne 0) "staging fast override was accepted: $invalidOutput"
    Assert (-not (Test-Path (Join-Path $oldEvidence 'stale.fixture'))) 'rejected tier retained stale evidence'
    $invalidSummary = Get-Content -Raw (Join-Path $oldEvidence 'run-summary.json') | ConvertFrom-Json
    Assert ($invalidSummary.status -eq 'failed' -and $invalidSummary.error -match 'require the full tier') 'rejected tier did not leave failure evidence'
    # Exercise the real CLI with the tier argument omitted, as the pending v1
    # reusable caller does. Stop before tool resolution/compilation and inspect
    # the chosen tier in fresh evidence; a full check must never default to fast.
    Set-Content -LiteralPath (Join-Path $source 'tools/windows/MatterWindowsToolchain.psm1') -Value @'
function Resolve-MatterWindowsToolchain { param([string]$RepositoryRoot); throw 'FULL_TIER_FIXTURE_STOP' }
Export-ModuleMember -Function Resolve-MatterWindowsToolchain
'@
    try {
        $env:GITHUB_REF = 'refs/heads/aq/integration/fixture'
        $ErrorActionPreference = 'Continue'
        $legacyOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File (Join-Path $source 'tools/ci-windows.ps1') -CiRoot (Join-Path $fixture 'legacy-ci') 2>&1 | Out-String
        $legacyExit = $LASTEXITCODE
    } finally {
        $env:GITHUB_REF = $savedRef
        $ErrorActionPreference = $savedErrorAction
    }
    $legacySummary = Get-Content -Raw (Join-Path $oldEvidence 'run-summary.json') | ConvertFrom-Json
    Assert ($legacyExit -ne 0 -and $legacySummary.error -match 'FULL_TIER_FIXTURE_STOP') "legacy fixture did not stop at tool resolution: $legacyOutput"
    Assert ($legacySummary.tier -eq 'full') 'legacy full-check caller silently selected fast'
    $plan = Get-MatterCiBuildPlan -RepositoryRoot $source -CiRoot $ciRoot -Fingerprint 'a'
    Assert $plan.Clean 'first run must clean'
    Initialize-MatterCiBuild $plan
    $marker = Join-Path $plan.BuildDirectory 'object.fixture'
    Set-Content -LiteralPath $marker -Value 'retained object'
    $partial = Get-MatterCiBuildPlan -RepositoryRoot $source -CiRoot $ciRoot -Fingerprint 'a'
    Assert ($partial.Reasons -contains 'incomplete_clean_build') 'partial build must not become warm'
    Complete-MatterCiBuild $plan
    $warm = Get-MatterCiBuildPlan -RepositoryRoot $source -CiRoot $ciRoot -Fingerprint 'a'
    Assert (-not $warm.Clean) 'unchanged successful build must be warm'
    Initialize-MatterCiBuild $warm
    Assert (Test-Path -LiteralPath $marker) 'warm initialization deleted an object'
    $changed = Get-MatterCiBuildPlan -RepositoryRoot $source -CiRoot $ciRoot -Fingerprint 'b'
    Assert ($changed.Reasons -contains 'toolchain_or_cmake_changed') 'changed build inputs must invalidate'
    Initialize-MatterCiBuild $changed
    Assert (-not (Test-Path -LiteralPath $marker)) 'invalidated object survived'
    Complete-MatterCiBuild $changed
    $nextDay = Get-MatterCiBuildPlan -RepositoryRoot $source -CiRoot $ciRoot -Fingerprint 'b' -UtcNow ([DateTime]::UtcNow.AddDays(1))
    Assert (-not $nextDay.Clean) 'date alone must not defeat persistent incremental builds'
    $forced = Get-MatterCiBuildPlan -RepositoryRoot $source -CiRoot $ciRoot -Fingerprint 'b' -ForceClean
    Assert ($forced.Reasons -contains 'forced_clean') 'nightly clean switch was ignored'
    $moved = Get-MatterCiBuildPlan -RepositoryRoot (Join-Path $fixture 'other-source') -CiRoot $ciRoot -Fingerprint 'b'
    Assert ($moved.Reasons -contains 'checkout_moved') 'different checkout must not reuse build files'
    Assert-Rejects { Get-MatterCiBuildPlan -RepositoryRoot $source -CiRoot $source -Fingerprint a } 'outside the checkout'
    $unowned = Join-Path $fixture 'unowned/build-relwithdebinfo'
    New-Item -ItemType Directory -Force -Path $unowned | Out-Null
    Set-Content (Join-Path $unowned 'valuable.fixture') 'retain'
    Assert-Rejects { Get-MatterCiBuildPlan -RepositoryRoot $source -CiRoot (Split-Path $unowned) -Fingerprint a } 'unowned'
    Assert (Test-Path (Join-Path $unowned 'valuable.fixture')) 'unowned content changed'
    $corruptCache = Join-Path $fixture 'cache/tools/sccache-fixture'
    New-Item -ItemType Directory -Force -Path $corruptCache | Out-Null
    Set-Content (Join-Path $corruptCache 'sccache.exe') 'wrong executable'
    Assert-Rejects { Install-MatterCiSccache -CiRoot (Join-Path $fixture 'cache') -Pin ([pscustomobject]@{version='fixture'; executable_sha256='bad'}) } 'does not match'
} finally { Remove-Item -LiteralPath $fixture -Recurse -Force }
Write-Host 'windows_ci_contract_tests: PASS'
