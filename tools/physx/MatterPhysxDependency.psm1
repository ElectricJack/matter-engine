Set-StrictMode -Version Latest

function Require-MatterPhysxFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description was not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Invoke-MatterPhysxGit {
    param(
        [Parameter(Mandatory = $true)][string]$PhysxRoot,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure
    )
    $output = @(& git.exe -C $PhysxRoot @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "PhysX git query '$($Arguments -join ' ')' failed:`n$($output -join "`n")"
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = ($output -join "`n").Trim()
    }
}

function Read-MatterPhysxLock {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$LockPath)

    $resolvedLock = Require-MatterPhysxFile -Path $LockPath -Description 'PhysX dependency lock'
    try {
        $lock = Get-Content -LiteralPath $resolvedLock -Raw | ConvertFrom-Json
    } catch {
        throw "PhysX dependency lock is not valid JSON: $resolvedLock ($($_.Exception.Message))"
    }
    $required = @(
        'schema', 'url', 'tag', 'commit', 'sdkVersion', 'physicsVersionHex',
        'cudaToolkitVersion', 'cudaNvccVersion', 'windowsPreset', 'binaryAbi', 'license'
    )
    foreach ($name in $required) {
        if (-not ($lock.PSObject.Properties.Name -contains $name) -or
            [string]::IsNullOrWhiteSpace([string]$lock.$name)) {
            throw "PhysX dependency lock is missing '$name': $resolvedLock"
        }
    }
    if ([int]$lock.schema -ne 1) {
        throw "Unsupported PhysX dependency lock schema '$($lock.schema)' (expected 1)"
    }
    if ([string]$lock.commit -notmatch '^[0-9a-f]{40}$') {
        throw "PhysX dependency lock commit must be a lowercase 40-character SHA: $($lock.commit)"
    }
    return $lock
}

function Assert-MatterPhysxGeneratorPath {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$PhysxRoot)

    $resolved = (Resolve-Path -LiteralPath $PhysxRoot).Path
    if ($resolved -match '\s') {
        throw "The PhysX 5.6.1 project generator does not safely support whitespace in its checkout path: $resolved"
    }
    return $resolved
}

function Resolve-MatterPhysxDependency {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$PhysxRoot,
        [Parameter(Mandatory = $true)][string]$CudaRoot,
        [string]$LockPath
    )

    $repository = (Resolve-Path -LiteralPath $RepositoryRoot).Path.TrimEnd('\', '/')
    $physx = (Resolve-Path -LiteralPath $PhysxRoot).Path.TrimEnd('\', '/')
    $cuda = (Resolve-Path -LiteralPath $CudaRoot).Path.TrimEnd('\', '/')
    if (-not $LockPath) {
        $LockPath = Join-Path $repository 'tools\deps\physx.lock.json'
    }
    $lock = Read-MatterPhysxLock -LockPath $LockPath

    $repositoryPrefix = $repository + [IO.Path]::DirectorySeparatorChar
    if ($physx.Equals($repository, [StringComparison]::OrdinalIgnoreCase) -or
        $physx.StartsWith($repositoryPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "The PhysX checkout must remain external; it is inside the Matter repository: $physx"
    }

    $sdkRoot = Join-Path $physx 'physx'
    $versionHeader = Require-MatterPhysxFile `
        -Path (Join-Path $sdkRoot 'include\foundation\PxPhysicsVersion.h') `
        -Description 'PhysX version header'
    Require-MatterPhysxFile -Path (Join-Path $sdkRoot "buildtools\presets\public\$($lock.windowsPreset).xml") -Description 'PhysX Windows preset' | Out-Null
    Require-MatterPhysxFile -Path (Join-Path $sdkRoot 'generate_projects.bat') -Description 'PhysX project generator' | Out-Null
    Require-MatterPhysxFile -Path (Join-Path $physx ([string]$lock.license)) -Description 'PhysX license' | Out-Null

    $head = (Invoke-MatterPhysxGit -PhysxRoot $physx -Arguments @('rev-parse', 'HEAD')).Output.ToLowerInvariant()
    if ($head -ne [string]$lock.commit) {
        throw "PhysX commit mismatch: expected $($lock.commit), found $head"
    }
    $symbolicHead = Invoke-MatterPhysxGit -PhysxRoot $physx -Arguments @('symbolic-ref', '--quiet', '--short', 'HEAD') -AllowFailure
    if ($symbolicHead.ExitCode -eq 0) {
        throw "PhysX checkout must use a detached HEAD at the locked commit; found branch '$($symbolicHead.Output)'"
    }
    $tagCommit = (Invoke-MatterPhysxGit -PhysxRoot $physx -Arguments @('rev-list', '-n', '1', "refs/tags/$($lock.tag)")).Output.ToLowerInvariant()
    if ($tagCommit -ne [string]$lock.commit) {
        throw "PhysX tag '$($lock.tag)' does not resolve to locked commit $($lock.commit); found $tagCommit"
    }
    $origin = (Invoke-MatterPhysxGit -PhysxRoot $physx -Arguments @('remote', 'get-url', 'origin')).Output.TrimEnd('/')
    $lockedUrl = ([string]$lock.url).TrimEnd('/')
    if ($origin -ne $lockedUrl) {
        throw "PhysX origin mismatch: expected $lockedUrl, found $origin"
    }

    $headerText = Get-Content -LiteralPath $versionHeader -Raw
    $components = @()
    foreach ($macro in @('MAJOR', 'MINOR', 'BUGFIX')) {
        $match = [regex]::Match($headerText, "(?m)^\s*#define\s+PX_PHYSICS_VERSION_$macro\s+(\d+)\s*$")
        if (-not $match.Success) {
            throw "PhysX version header is missing PX_PHYSICS_VERSION_$macro"
        }
        $components += [int]$match.Groups[1].Value
    }
    $sdkVersion = $components -join '.'
    $versionHex = '0x{0:X2}{1:X2}{2:X2}00' -f $components[0], $components[1], $components[2]
    if ($sdkVersion -ne [string]$lock.sdkVersion -or $versionHex -ne [string]$lock.physicsVersionHex) {
        throw "PhysX SDK header mismatch: expected $($lock.sdkVersion)/$($lock.physicsVersionHex), found $sdkVersion/$versionHex"
    }

    $cudaVersionPath = Require-MatterPhysxFile -Path (Join-Path $cuda 'version.json') -Description 'CUDA Toolkit version manifest'
    try {
        $cudaManifest = Get-Content -LiteralPath $cudaVersionPath -Raw | ConvertFrom-Json
        $cudaToolkitVersion = ([string]$cudaManifest.cuda.version -split '\.')[0..1] -join '.'
        $cudaVersion = [string]$cudaManifest.cuda_nvcc.version
    } catch {
        throw "CUDA Toolkit version manifest is invalid: $cudaVersionPath ($($_.Exception.Message))"
    }
    if ($cudaToolkitVersion -ne [string]$lock.cudaToolkitVersion) {
        throw "CUDA Toolkit version mismatch: expected $($lock.cudaToolkitVersion), found $cudaToolkitVersion"
    }
    if ($cudaVersion -ne [string]$lock.cudaNvccVersion) {
        throw "CUDA NVCC version mismatch: expected $($lock.cudaNvccVersion), found $cudaVersion"
    }

    $binaryDirectory = Join-Path $sdkRoot "bin\$($lock.binaryAbi)\release"
    return [pscustomobject][ordered]@{
        RepositoryRoot = $repository
        PhysxRoot = $physx
        SdkRoot = $sdkRoot
        CudaRoot = $cuda
        Commit = $head
        Tag = [string]$lock.tag
        SdkVersion = $sdkVersion
        PhysicsVersionHex = $versionHex
        CudaToolkitVersion = $cudaToolkitVersion
        CudaVersion = $cudaVersion
        WindowsPreset = [string]$lock.windowsPreset
        BinaryDirectory = $binaryDirectory
        ControlExecutable = Join-Path $binaryDirectory 'SnippetPBF_64.exe'
        GpuRuntime = Join-Path $binaryDirectory 'PhysXGpu_64.dll'
        LicensePath = Join-Path $physx ([string]$lock.license)
    }
}

Export-ModuleMember -Function `
    Assert-MatterPhysxGeneratorPath, `
    Read-MatterPhysxLock, `
    Resolve-MatterPhysxDependency
