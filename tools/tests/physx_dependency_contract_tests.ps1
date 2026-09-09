[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$modulePath = Join-Path $repositoryRoot 'tools\physx\MatterPhysxDependency.psm1'
$runnerPath = Join-Path $repositoryRoot 'tools\physx\build-physx-control.ps1'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("matter-physx-contract-" + [guid]::NewGuid().ToString('N'))

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-Equal($Actual, $Expected, [string]$Message) {
    if ($Actual -ne $Expected) {
        throw "$Message (expected '$Expected', got '$Actual')"
    }
}

function Assert-Throws([scriptblock]$Action, [string]$Pattern, [string]$Message) {
    try {
        & $Action
    } catch {
        if ($_.Exception.Message -notmatch $Pattern) {
            throw "$Message (unexpected error: $($_.Exception.Message))"
        }
        return
    }
    throw "$Message (no error was raised)"
}

function Invoke-Git([string]$WorkingDirectory, [string[]]$Arguments) {
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& git.exe -C $WorkingDirectory @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($exitCode -ne 0) {
        throw "git $($Arguments -join ' ') failed:`n$($output -join "`n")"
    }
    return ($output -join "`n").Trim()
}

function New-FakePhysxCheckout([string]$Path) {
    New-Item -ItemType Directory -Force -Path `
        (Join-Path $Path 'physx\include\foundation'), `
        (Join-Path $Path 'physx\buildtools\presets\public') | Out-Null
    Set-Content -LiteralPath (Join-Path $Path 'physx\include\foundation\PxPhysicsVersion.h') -Encoding ascii -Value @'
#define PX_PHYSICS_VERSION_MAJOR 5
#define PX_PHYSICS_VERSION_MINOR 6
#define PX_PHYSICS_VERSION_BUGFIX 1
#define PX_PHYSICS_VERSION ((PX_PHYSICS_VERSION_MAJOR<<24) + (PX_PHYSICS_VERSION_MINOR<<16) + (PX_PHYSICS_VERSION_BUGFIX<<8) + 0)
'@
    Set-Content -LiteralPath (Join-Path $Path 'physx\buildtools\presets\public\vc17win64.xml') -Encoding ascii -Value '<preset name="vc17win64" />'
    Set-Content -LiteralPath (Join-Path $Path 'physx\generate_projects.bat') -Encoding ascii -Value '@exit /b 0'
    Set-Content -LiteralPath (Join-Path $Path 'LICENSE.md') -Encoding ascii -Value 'PhysX fixture license'
    & git.exe -C $Path init --quiet
    if ($LASTEXITCODE -ne 0) { throw 'failed to initialize fake PhysX checkout' }
    Invoke-Git $Path @('add', '.') | Out-Null
    Invoke-Git $Path @('-c', 'user.name=Matter Tests', '-c', 'user.email=matter-tests@example.invalid', 'commit', '--quiet', '-m', 'fixture') | Out-Null
    $commit = Invoke-Git $Path @('rev-parse', 'HEAD')
    Invoke-Git $Path @('tag', '107.3-physx-5.6.1') | Out-Null
    Invoke-Git $Path @('remote', 'add', 'origin', 'https://github.com/NVIDIA-Omniverse/PhysX.git') | Out-Null
    Invoke-Git $Path @('checkout', '--quiet', '--detach', $commit) | Out-Null
    return $commit
}

function Write-Lock([string]$Path, [string]$Commit, [string]$CudaNvccVersion = '12.8.61') {
    [ordered]@{
        schema = 1
        url = 'https://github.com/NVIDIA-Omniverse/PhysX.git'
        tag = '107.3-physx-5.6.1'
        commit = $Commit
        sdkVersion = '5.6.1'
        physicsVersionHex = '0x05060100'
        cudaToolkitVersion = '12.8'
        cudaNvccVersion = $CudaNvccVersion
        windowsPreset = 'vc17win64'
        binaryAbi = 'win.x86_64.vc143.mt'
        license = 'LICENSE.md'
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $Path -Encoding utf8
}

try {
    New-Item -ItemType Directory -Force -Path $scratch | Out-Null
    Import-Module $modulePath -Force

    $matterRoot = Join-Path $scratch 'matter-repository'
    $physxRoot = Join-Path $scratch 'physx-checkout'
    $cudaRoot = Join-Path $scratch 'cuda-12.8'
    New-Item -ItemType Directory -Force -Path $matterRoot, $physxRoot, $cudaRoot | Out-Null
    $commit = New-FakePhysxCheckout $physxRoot
    Set-Content -LiteralPath (Join-Path $cudaRoot 'version.json') -Encoding utf8 -Value @'
{
  "cuda": { "name": "CUDA SDK", "version": "12.8.0" },
  "cuda_nvcc": { "name": "CUDA NVCC", "version": "12.8.61" }
}
'@
    $lockPath = Join-Path $scratch 'physx.lock.json'
    Write-Lock $lockPath $commit

    $resolved = Resolve-MatterPhysxDependency `
        -RepositoryRoot $matterRoot `
        -PhysxRoot $physxRoot `
        -CudaRoot $cudaRoot `
        -LockPath $lockPath
    Assert-Equal $resolved.Commit $commit 'exact PhysX commit was not preserved'
    Assert-Equal $resolved.SdkVersion '5.6.1' 'SDK header version was not resolved'
    Assert-Equal $resolved.CudaToolkitVersion '12.8' 'CUDA Toolkit family was not resolved'
    Assert-Equal $resolved.CudaVersion '12.8.61' 'CUDA compiler version was not resolved'
    Assert-Equal $resolved.BinaryDirectory (Join-Path $physxRoot 'physx\bin\win.x86_64.vc143.mt\release') 'binary directory did not follow the locked ABI'

    $wrongCommitLock = Join-Path $scratch 'wrong-commit.lock.json'
    Write-Lock $wrongCommitLock ('0' * 40)
    Assert-Throws {
        Resolve-MatterPhysxDependency -RepositoryRoot $matterRoot -PhysxRoot $physxRoot -CudaRoot $cudaRoot -LockPath $wrongCommitLock
    } 'commit.*mismatch|expected.*0000' 'wrong PhysX commit was accepted'

    $wrongCudaLock = Join-Path $scratch 'wrong-cuda.lock.json'
    Write-Lock $wrongCudaLock $commit '12.9.0'
    Assert-Throws {
        Resolve-MatterPhysxDependency -RepositoryRoot $matterRoot -PhysxRoot $physxRoot -CudaRoot $cudaRoot -LockPath $wrongCudaLock
    } 'CUDA.*mismatch|12\.9\.0' 'wrong CUDA version was accepted'

    $insideMatter = Join-Path $matterRoot 'external\PhysX'
    New-Item -ItemType Directory -Force -Path $insideMatter | Out-Null
    $insideCommit = New-FakePhysxCheckout $insideMatter
    $insideLock = Join-Path $scratch 'inside.lock.json'
    Write-Lock $insideLock $insideCommit
    Assert-Throws {
        Resolve-MatterPhysxDependency -RepositoryRoot $matterRoot -PhysxRoot $insideMatter -CudaRoot $cudaRoot -LockPath $insideLock
    } 'external|inside.*repository' 'PhysX checkout inside Matter repository was accepted'

    $spacedRoot = Join-Path $scratch 'physx checkout with spaces'
    New-Item -ItemType Directory -Force -Path $spacedRoot | Out-Null
    Assert-Throws {
        Assert-MatterPhysxGeneratorPath -PhysxRoot $spacedRoot
    } 'whitespace|spaces' 'generator path containing whitespace was accepted'

    $runnerOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runnerPath `
        -Mode Validate -RepositoryRoot $matterRoot -PhysxRoot $physxRoot -CudaRoot $cudaRoot -LockPath $lockPath 2>&1)
    Assert-True ($LASTEXITCODE -eq 0) "offline Validate runner failed:`n$($runnerOutput -join "`n")"
    Assert-True (($runnerOutput -join "`n") -match 'MATTER_PHYSX_VALIDATE=PASS') 'Validate runner omitted its PASS evidence'

    $riverProbe = Join-Path $scratch 'river_runtime_dependency_probe.cpp'
    Set-Content -LiteralPath $riverProbe -Encoding ascii -Value @'
#include "matter/river_runtime.h"
int main() { matter::RiverRuntimeBinding binding; return binding.generation() == 0 ? 0 : 1; }
'@
    $riverInclude = Join-Path $repositoryRoot 'MatterEngine3\include'
    $riverCompiler = Get-Command cl.exe -ErrorAction SilentlyContinue
    $riverCommand = $null
    if (-not $riverCompiler) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} `
            'Microsoft Visual Studio\Installer\vswhere.exe'
        Assert-True (Test-Path -LiteralPath $vswhere) `
            'Visual Studio discovery tool was not found'
        $installation = (& $vswhere -latest -products '*' `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath | Select-Object -First 1)
        $vsDevCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
        Assert-True (Test-Path -LiteralPath $vsDevCmd) `
            'Visual Studio developer environment was not found'
        $riverCommand = 'call "{0}" -no_logo -arch=x64 >nul && cl.exe /nologo /std:c++17 /Zs /showIncludes /I"{1}" "{2}"' -f `
            $vsDevCmd, $riverInclude, $riverProbe
    }
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($riverCompiler) {
            $riverOutput = @(& $riverCompiler.Source /nologo /std:c++17 /Zs `
                /showIncludes "/I$riverInclude" $riverProbe 2>&1)
        } else {
            $riverOutput = @(& cmd.exe /d /s /c $riverCommand 2>&1)
        }
        $riverExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    Assert-True ($riverExitCode -eq 0) "public river runtime probe failed to compile:`n$($riverOutput -join "`n")"
    $riverDependencies = ($riverOutput -join "`n")
    Assert-True ($riverDependencies -notmatch '(?i)(physx|vulkan|flecs|[\\/]provider[\\/]|[\\/]render[\\/]|[\\/]hydrology[\\/])') `
        'public river runtime header leaked a provider, solver, renderer, or ECS dependency'

    Write-Output 'PhysX dependency contract: PASS (exact lock, external checkout, offline validation, safe generator path, provider-free river runtime API)'
} finally {
    if (Test-Path -LiteralPath $scratch) {
        Remove-Item -LiteralPath $scratch -Recurse -Force
    }
}
