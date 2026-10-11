Set-StrictMode -Version Latest

function Get-MatterCiTier {
    param([string]$Ref, [string]$BaseRef, [string]$EventName,
        [ValidateSet('auto', 'fast', 'full')][string]$Tier = 'auto', [switch]$RequireExplicitTier)
    if ($RequireExplicitTier -and $Tier -eq 'auto') { throw 'Reusable CI requires an explicit fast or full tier.' }
    $fullRequired = $Ref -in @('refs/heads/staging', 'refs/heads/main') -or
        $BaseRef -in @('staging', 'main', 'refs/heads/staging', 'refs/heads/main') -or $EventName -eq 'schedule'
    if ($fullRequired -and $Tier -eq 'fast') { throw 'staging, main and scheduled runs require the full tier.' }
    if ($Tier -ne 'auto') { return $Tier }
    if ($fullRequired -or -not $Ref) { return 'full' }
    # Existing AQ candidate and PR qualification uses the full-v1 check set.
    # Only the direct dev feedback run is fast/advisory until trust support exists.
    if ($Ref -eq 'refs/heads/dev') { return 'fast' }
    return 'full'
}

function Get-MatterCiFingerprint {
    param([string]$RepositoryRoot, $Toolchain)
    $inputs = @('CMakeLists.txt', 'CMakePresets.json', 'tools/build-windows.ps1')
    $inputs += @(Get-ChildItem (Join-Path $RepositoryRoot 'cmake') -Recurse -File |
        Where-Object { $_.Name -eq 'CMakeLists.txt' -or $_.Extension -in @('.cmake', '.sources') } |
        ForEach-Object { $_.FullName.Substring($RepositoryRoot.Length + 1).Replace('\', '/') })
    $inputs += @(Get-ChildItem (Join-Path $RepositoryRoot 'tools/windows') -File |
        Where-Object { $_.Extension -in @('.psm1', '.json') } |
        ForEach-Object { $_.FullName.Substring($RepositoryRoot.Length + 1).Replace('\', '/') })
    $rows = @($inputs | Sort-Object -Unique | ForEach-Object {
        '{0}:{1}' -f $_, (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $RepositoryRoot $_)).Hash
    })
    # Tool paths and binary hashes catch an in-place tool upgrade even when the
    # checked-in lock is unchanged. No commit SHA: ordinary edits stay incremental.
    $rows += ($Toolchain | ConvertTo-Json -Depth 4 -Compress)
    foreach ($path in @($Toolchain.CMake, $Toolchain.Ninja, $Toolchain.Glslc,
            (Join-Path $Toolchain.VisualStudioRoot "VC/Tools/MSVC/$($Toolchain.MsvcToolsVersion)/bin/Hostx64/x64/cl.exe"))) {
        $rows += "$path`:$((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash)"
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $key = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($rows -join "`n")))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    [pscustomobject]@{ Key = $key; Inputs = $rows }
}

function Get-MatterCiBuildPlan {
    param([string]$RepositoryRoot, [string]$CiRoot, [string]$Fingerprint,
        [string]$Config = 'RelWithDebInfo', [DateTime]$UtcNow = [DateTime]::UtcNow, [switch]$ForceClean)
    $root = [IO.Path]::GetFullPath($CiRoot).TrimEnd('\', '/')
    if ($root.StartsWith('\\') -or $root.Length -lt 4 -or $root[1] -ne ':') {
        throw 'CI root must be a directory on a local Windows drive.'
    }
    $source = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\', '/')
    if ($root.StartsWith($source + '\', [StringComparison]::OrdinalIgnoreCase) -or $root -eq $source) {
        throw 'CI root must be outside the checkout.'
    }
    if ($Config -notin @('Debug', 'RelWithDebInfo', 'Release')) { throw 'Unsupported CI configuration.' }
    $build = Join-Path $root "build-$($Config.ToLowerInvariant())"
    $statePath = "$build.state.json"
    $state = $null
    if (Test-Path -LiteralPath $statePath) {
        $state = Get-Content -Raw -LiteralPath $statePath | ConvertFrom-Json
        if ($state.schema -ne 'matter.windows-ci-state.v1') { throw 'Unrecognized CI build ownership marker.' }
    } elseif ((Test-Path -LiteralPath $build) -and @(Get-ChildItem -LiteralPath $build -Force).Count) {
        throw "Refusing an unowned nonempty CI build directory: $build"
    }
    foreach ($path in @($root, $build)) {
        if ((Test-Path -LiteralPath $path) -and
            ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "CI directories must not be reparse points: $path"
        }
    }
    $reasons = @()
    if (-not $state) { $reasons += 'first_build' }
    else {
        if ($state.fingerprint -ne $Fingerprint) { $reasons += 'toolchain_or_cmake_changed' }
        if ($state.repository_root -ne $source) { $reasons += 'checkout_moved' }
        if (-not $state.last_clean_utc) { $reasons += 'incomplete_clean_build' }
        elseif ([DateTime]::Parse($state.last_clean_utc).ToUniversalTime().Date -lt $UtcNow.Date) { $reasons += 'daily_clean' }
    }
    if ($ForceClean) { $reasons += 'forced_clean' }
    [pscustomobject]@{ Root = $root; BuildDirectory = $build; StatePath = $statePath;
        RepositoryRoot = $source; Fingerprint = $Fingerprint; Clean = ($reasons.Count -gt 0);
        Reasons = $reasons; PreviousState = $state }
}

function Initialize-MatterCiBuild {
    param($Plan)
    New-Item -ItemType Directory -Force -Path $Plan.Root | Out-Null
    if ($Plan.Clean -and (Test-Path -LiteralPath $Plan.BuildDirectory)) {
        Remove-Item -LiteralPath $Plan.BuildDirectory -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $Plan.BuildDirectory | Out-Null
    $cleanAt = if (-not $Plan.Clean -and $Plan.PreviousState) { $Plan.PreviousState.last_clean_utc } else { $null }
    [ordered]@{ schema = 'matter.windows-ci-state.v1'; repository_root = $Plan.RepositoryRoot;
        fingerprint = $Plan.Fingerprint; last_clean_utc = $cleanAt } |
        ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath $Plan.StatePath
}

function Complete-MatterCiBuild {
    param($Plan)
    $state = Get-Content -Raw -LiteralPath $Plan.StatePath | ConvertFrom-Json
    if ($Plan.Clean) { $state.last_clean_utc = [DateTime]::UtcNow.ToString('o') }
    $state | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath $Plan.StatePath
}

function Install-MatterCiSccache {
    param([string]$CiRoot, $Pin)
    $directory = Join-Path $CiRoot "tools/sccache-$($Pin.version)"
    $executable = Join-Path $directory 'sccache.exe'
    if (Test-Path -LiteralPath $executable) {
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash -ine $Pin.executable_sha256) {
            throw 'Installed sccache does not match the toolchain lock.'
        }
        return $executable
    }
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $archive = Join-Path $directory 'release.zip'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri $Pin.url -OutFile $archive
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ine $Pin.archive_sha256) {
        throw 'Downloaded sccache archive does not match the toolchain lock.'
    }
    $unpack = Join-Path $directory 'unpack'
    Expand-Archive -LiteralPath $archive -DestinationPath $unpack -Force
    $candidates = @(Get-ChildItem -LiteralPath $unpack -Recurse -Filter sccache.exe -File)
    if ($candidates.Count -ne 1 -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $candidates[0].FullName).Hash -ine $Pin.executable_sha256) {
        throw 'Extracted sccache executable does not match the toolchain lock.'
    }
    Copy-Item -LiteralPath $candidates[0].FullName -Destination $executable
    return $executable
}

Export-ModuleMember -Function Get-MatterCiTier, Get-MatterCiFingerprint, Get-MatterCiBuildPlan,
    Initialize-MatterCiBuild, Complete-MatterCiBuild, Install-MatterCiSccache
