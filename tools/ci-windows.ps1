[CmdletBinding()]
param(
    [ValidateSet('auto', 'fast', 'full')][string]$Tier = 'full',
    [string]$CiRoot = 'C:\matter-ci\matter-engine',
    [switch]$ForceClean,
    [switch]$DisableCompilerCache,
    [switch]$RequireExplicitTier,
    [switch]$Package
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Import-Module (Join-Path $PSScriptRoot 'windows/MatterWindowsCi.psm1') -Force
$evidence = Join-Path $repositoryRoot 'artifacts/ci'
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
# A self-hosted checkout may retain ignored artifacts. Upload only this run.
Get-ChildItem -LiteralPath $evidence -File | Remove-Item -Force
$summary = [ordered]@{ schema = 'matter.windows-ci-run.v1'; source_sha = $env:MATTER_CI_SOURCE_SHA;
    hosted = ($env:GITHUB_ACTIONS -eq 'true'); ci_script_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $PSCommandPath).Hash.ToLowerInvariant();
    ref = $env:GITHUB_REF; event = $env:GITHUB_EVENT_NAME; requested_tier = $Tier; tier = $null;
    started_at_utc = [DateTime]::UtcNow.ToString('o'); status = 'failed'; phases = @() }
$total = [Diagnostics.Stopwatch]::StartNew()
$transcribing = $false
$lock = $null
$cache = $null
$cacheStarted = $false
$failure = $null

function Invoke-CiPhase([string]$Name, [scriptblock]$Action) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $passed = $false
    try { & $Action; $passed = $true }
    finally {
        $summary.phases += [ordered]@{ name = $Name; seconds = $watch.Elapsed.TotalSeconds; passed = $passed }
    }
}

Push-Location $repositoryRoot
try {
    Start-Transcript -Path (Join-Path $evidence 'native-windows.log') -Force | Out-Null
    $transcribing = $true
    $selectedTier = Get-MatterCiTier -Ref $env:GITHUB_REF -BaseRef $env:GITHUB_BASE_REF `
        -EventName $env:GITHUB_EVENT_NAME -Tier $Tier -RequireExplicitTier:$RequireExplicitTier
    $summary.tier = $selectedTier
    if (-not $summary.source_sha) { $summary.source_sha = $env:GITHUB_SHA }
    $summary.package_required = Get-MatterCiPackageRequired -Ref $env:GITHUB_REF `
        -EventName $env:GITHUB_EVENT_NAME -Tier $selectedTier -Package:$Package
    $testLabel = if ($summary.package_required) { 'ci-full' } elseif ($selectedTier -eq 'fast') { 'ci-fast' } else { 'ci-pr' }
    $summary.test_label = $testLabel
    if ($env:GITHUB_EVENT_NAME -eq 'schedule') {
        $ForceClean = $true
        $DisableCompilerCache = $true
    }
    if (-not $env:CMAKE_BUILD_PARALLEL_LEVEL) { $env:CMAKE_BUILD_PARALLEL_LEVEL = '4' }
    if (-not $env:CTEST_PARALLEL_LEVEL) { $env:CTEST_PARALLEL_LEVEL = [string][Environment]::ProcessorCount }
    $summary.build_parallelism = $env:CMAKE_BUILD_PARALLEL_LEVEL
    $summary.test_parallelism = $env:CTEST_PARALLEL_LEVEL
    if (-not $env:VK_LOADER_LAYERS_DISABLE) {
        $env:VK_LOADER_LAYERS_DISABLE = 'VK_LAYER_OBS_HOOK,VK_LAYER_EOS_Overlay,' +
            'VK_LAYER_VALVE_steam_overlay,VK_LAYER_VALVE_steam_fossilize,VK_LAYER_RENDERDOC_Capture'
    }
    $CiRoot = [IO.Path]::GetFullPath($CiRoot)
    New-Item -ItemType Directory -Force -Path $CiRoot | Out-Null
    # Per-ref Actions cancellation does not protect a tree across different refs.
    $lock = [IO.File]::Open((Join-Path $CiRoot 'ci.lock'), [IO.FileMode]::OpenOrCreate,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    Import-Module (Join-Path $PSScriptRoot 'windows/MatterWindowsToolchain.psm1') -Force
    $toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot
    $pin = Get-Content -Raw (Join-Path $PSScriptRoot 'windows/toolchain.lock.json') | ConvertFrom-Json
    $cmakeVersion = ((& $toolchain.CMake --version | Select-Object -First 1) -replace '^cmake version ', '').Trim()
    $ninjaVersion = (& $toolchain.Ninja --version | Out-String).Trim()
    if ($toolchain.MsvcToolsVersion -ne $pin.msvc_tools -or
        $toolchain.WindowsSdkVersion -ne $pin.windows_sdk -or
        (Split-Path $toolchain.VulkanSdk -Leaf) -ne $pin.vulkan_sdk -or
        $toolchain.PythonVersion -ne "Python $($pin.python)" -or
        $cmakeVersion -ne $pin.cmake -or $ninjaVersion -ne $pin.ninja) {
        throw 'Resolved native toolchain does not match tools/windows/toolchain.lock.json.'
    }
    $toolchain | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $evidence 'toolchain.json')
    $fingerprint = Get-MatterCiFingerprint -RepositoryRoot $repositoryRoot -Toolchain $toolchain
    $plan = Get-MatterCiBuildPlan -RepositoryRoot $repositoryRoot -CiRoot $CiRoot -Fingerprint $fingerprint.Key -ForceClean:$ForceClean
    $summary.build_directory = $plan.BuildDirectory
    $summary.clean = $plan.Clean
    $summary.clean_reasons = $plan.Reasons
    $summary.fingerprint = $fingerprint.Key
    $fingerprint | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $evidence 'build-inputs.json')
    Initialize-MatterCiBuild -Plan $plan
    $buildArgs = @{ Config = 'RelWithDebInfo'; BuildDirectory = $plan.BuildDirectory }
    $summary.compiler_cache_enabled = -not $DisableCompilerCache
    if (-not $DisableCompilerCache) {
        $cache = Install-MatterCiSccache -CiRoot $CiRoot -Pin $pin.sccache
        $lockHash = (Get-FileHash -Algorithm SHA256 (Join-Path $PSScriptRoot 'windows/toolchain.lock.json')).Hash.ToLowerInvariant()
        $env:SCCACHE_DIR = Join-Path $CiRoot "sccache/$lockHash"
        $env:SCCACHE_CACHE_SIZE = '20G'
        $portHash = [Security.Cryptography.SHA256]::Create()
        try { $portBytes = $portHash.ComputeHash([Text.Encoding]::UTF8.GetBytes($CiRoot)) }
        finally { $portHash.Dispose() }
        $env:SCCACHE_SERVER_PORT = [string](20000 + ([BitConverter]::ToUInt16($portBytes, 0) % 30000))
        # Retire a server orphaned by Actions cancellation under the exclusive
        # CI root lock. A missing server is normal on the first run.
        $savedErrorAction = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        & $cache --stop-server *> $null
        $ErrorActionPreference = $savedErrorAction
        & $cache --start-server
        if ($LASTEXITCODE -ne 0) { throw 'sccache server startup failed.' }
        $cacheStarted = $true
        & $cache --zero-stats
        if ($LASTEXITCODE -ne 0) { throw 'sccache statistics reset failed.' }
        $buildArgs.CompilerLauncher = $cache
        $summary.sccache_version = $pin.sccache.version
        $summary.sccache_directory = $env:SCCACHE_DIR
    }
    Invoke-CiPhase 'configure' {
        & (Join-Path $PSScriptRoot 'build-windows.ps1') @buildArgs -ConfigureOnly
        if ($LASTEXITCODE -ne 0) { throw "Native configure failed: $LASTEXITCODE" }
    }
    $ctest = Join-Path (Split-Path $toolchain.CMake) 'ctest.exe'
    & $ctest --test-dir $plan.BuildDirectory --show-only=json-v1 |
        Set-Content -Encoding UTF8 (Join-Path $evidence 'ctest-inventory.json')
    if ($LASTEXITCODE -ne 0) { throw 'CTest inventory failed.' }
    Invoke-CiPhase 'build' {
        if ($selectedTier -eq 'fast') {
            & (Join-Path $PSScriptRoot 'build-windows.ps1') @buildArgs -Target matter_ci_fast
        } else {
            & (Join-Path $PSScriptRoot 'build-windows.ps1') @buildArgs
        }
        if ($LASTEXITCODE -ne 0) { throw "Native build failed: $LASTEXITCODE" }
        Complete-MatterCiBuild -Plan $plan
    }
    $testFailure = $null
    $lintFailure = $null
    try { Invoke-CiPhase 'lint' {
        & (Join-Path $PSScriptRoot 'build-windows.ps1') @buildArgs -Target matter_ci_lint
        if ($LASTEXITCODE -ne 0) { throw "CI lint failed: $LASTEXITCODE" }
        & $ctest --test-dir $plan.BuildDirectory --show-only=json-v1 |
            Set-Content -Encoding UTF8 (Join-Path $evidence 'ctest-inventory.json')
        if ($LASTEXITCODE -ne 0) { throw 'Post-build CTest inventory failed.' }
        $lintArgs = @((Join-Path $PSScriptRoot 'ci-source-lint.py'),
            '--inventory', (Join-Path $evidence 'ctest-inventory.json'),
            '--output', (Join-Path $evidence 'source-lint.json'))
        if ([IO.Path]::GetFileName($toolchain.Python) -ieq 'py.exe') { $lintArgs = @('-3.13') + $lintArgs }
        & $toolchain.Python @lintArgs
        if ($LASTEXITCODE -ne 0) { throw "Source lint failed: $LASTEXITCODE" }
        if (-not (Test-Path -LiteralPath (Join-Path $evidence 'source-lint.json'))) {
            throw 'Source lint did not produce current evidence.'
        }
        $lintEvidence = Get-Content -LiteralPath (Join-Path $evidence 'source-lint.json') -Raw | ConvertFrom-Json
        if ($lintEvidence.status -ne 'passed') { throw 'Source lint evidence does not record a pass.' }
    } } catch { $lintFailure = $_ }
    try {
        Invoke-CiPhase 'ctest' {
            & $ctest --test-dir $plan.BuildDirectory -L "^$testLabel`$" --no-tests=error --output-on-failure `
                --output-junit (Join-Path $evidence 'ctest.xml')
            if ($LASTEXITCODE -ne 0) { throw "CTest failed: $LASTEXITCODE" }
        }
    } catch { $testFailure = $_ }
    if ($summary.package_required) {
        Invoke-CiPhase 'package' {
            & (Join-Path $PSScriptRoot 'build-windows.ps1') @buildArgs -Target matter_ci_promotion_lint
            if ($LASTEXITCODE -ne 0) { throw "Promotion shader lint failed: $LASTEXITCODE" }
            & (Join-Path $PSScriptRoot 'build-windows.ps1') @buildArgs -Target matter_dist
            if ($LASTEXITCODE -ne 0) { throw "Package build failed: $LASTEXITCODE" }
            & (Join-Path $PSScriptRoot 'check-windows-msvc-package.ps1') -DistPath 'MatterEditor/build/dist/world_demo'
            if ($LASTEXITCODE -ne 0) { throw "Package validation failed: $LASTEXITCODE" }
        }
    }
    if ($testFailure) { throw $testFailure }
    if ($lintFailure) { throw $lintFailure }
    $summary.status = 'passed'
} catch {
    $failure = $_
    $summary.error = $_.Exception.Message
} finally {
    if ($cacheStarted) {
        try {
            & $cache --show-stats --stats-format json | Set-Content -Encoding UTF8 (Join-Path $evidence 'sccache-stats.json')
            if ($LASTEXITCODE -ne 0) { throw 'sccache statistics collection failed.' }
            & $cache --show-stats | Set-Content -Encoding UTF8 (Join-Path $evidence 'sccache-stats.txt')
            & $cache --stop-server | Out-Null
        } catch {
            if (-not $failure) { $failure = $_; $summary.error = $_.Exception.Message }
            $summary.status = 'failed'
        }
    }
    if (Test-Path -LiteralPath (Join-Path $evidence 'ctest.xml')) {
        try {
            $pythonArgs = @((Join-Path $PSScriptRoot 'ci-test-report.py'), '--junit', (Join-Path $evidence 'ctest.xml'),
                '--inventory', (Join-Path $evidence 'ctest-inventory.json'), '--output', (Join-Path $evidence 'test-timings.json'),
                '--label', $testLabel, '--parallelism', $env:CTEST_PARALLEL_LEVEL)
            if ([IO.Path]::GetFileName($toolchain.Python) -ieq 'py.exe') { $pythonArgs = @('-3.13') + $pythonArgs }
            & $toolchain.Python @pythonArgs
            if ($LASTEXITCODE -ne 0) { throw 'CTest timing report failed.' }
        } catch {
            if (-not $failure) { $failure = $_; $summary.error = $_.Exception.Message }
            $summary.status = 'failed'
        }
    } elseif (@($summary.phases | Where-Object { $_.name -eq 'ctest' }).Count) {
        $missingEvidence = [System.Exception]::new('CTest did not produce current JUnit evidence.')
        if (-not $failure) { $failure = $missingEvidence; $summary.error = $missingEvidence.Message }
        $summary.status = 'failed'
    }
    $summary.finished_at_utc = [DateTime]::UtcNow.ToString('o')
    $summary.total_seconds = $total.Elapsed.TotalSeconds
    $summary.qualification_under_60_seconds = ($summary.status -eq 'passed' -and $summary.total_seconds -lt 60)
    $buildPhase = @($summary.phases | Where-Object { $_.name -eq 'build' })
    $testPhase = @($summary.phases | Where-Object { $_.name -eq 'ctest' })
    if ($testPhase.Count) {
        $summary.test_seconds = $testPhase[0].seconds
        $summary.build_and_test_seconds = $testPhase[0].seconds
        if ($buildPhase.Count) { $summary.build_and_test_seconds += $buildPhase[0].seconds }
        $summary.test_under_60_seconds = $summary.test_seconds -lt 60
        $summary.build_and_test_under_60_seconds = $summary.build_and_test_seconds -lt 60
    }
    $summary | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 (Join-Path $evidence 'run-summary.json')
    if ($transcribing) { Stop-Transcript | Out-Null }
    if ($lock) { $lock.Dispose() }
    Pop-Location
}
if ($failure) { throw $failure }
