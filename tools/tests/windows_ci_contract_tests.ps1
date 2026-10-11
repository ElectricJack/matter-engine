$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Import-Module (Join-Path $repository 'tools/windows/MatterWindowsCi.psm1') -Force
function Assert([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Assert-Rejects([scriptblock]$Action, [string]$Pattern) {
    try { & $Action; throw 'Expected rejection was absent.' }
    catch { if ($_.Exception.Message -notmatch $Pattern) { throw } }
}
Assert ((Get-MatterCiTier -Ref 'refs/heads/dev') -eq 'fast') 'dev must use fast'
Assert ((Get-MatterCiTier -Ref 'refs/heads/aq/batches/test') -eq 'fast') 'batches must use fast'
Assert ((Get-MatterCiTier -Ref 'refs/heads/staging') -eq 'full') 'staging must use full'
Assert ((Get-MatterCiTier -Ref 'refs/heads/main') -eq 'full') 'main must use full'
Assert ((Get-MatterCiTier -Ref 'refs/heads/dev' -EventName schedule) -eq 'full') 'nightly must use full'
Assert-Rejects { Get-MatterCiTier -Ref 'refs/heads/main' -Tier fast } 'require the full tier'

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
    Assert ($nextDay.Reasons -contains 'daily_clean') 'next UTC day must clean'
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
