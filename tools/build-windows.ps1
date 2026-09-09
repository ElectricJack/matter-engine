[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',
    [string]$Target,
    [switch]$EnablePhysx,
    [string]$PhysxRoot = $env:MATTER_PHYSX_ROOT,
    [string]$CudaRoot = $env:CUDA_PATH_V12_8,
    [string]$HydrologyCache,
    [switch]$PreflightOnly
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$modulePath = Join-Path $PSScriptRoot 'windows\MatterWindowsToolchain.psm1'
Import-Module $modulePath -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot

if ($PreflightOnly) {
    $toolchain | ConvertTo-Json -Depth 2
    exit 0
}

$preset = "windows-msvc-$($Config.ToLowerInvariant())"
$developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
$configure = '{0} && "{1}" --preset "{2}" -DCMAKE_MAKE_PROGRAM="{3}" -DMATTER_PYTHON_EXECUTABLE:FILEPATH="{4}"' -f $developerEnvironment, $toolchain.CMake, $preset, $toolchain.Ninja, $toolchain.Python
if ($EnablePhysx) {
    if (-not $PhysxRoot) {
        throw '-EnablePhysx requires -PhysxRoot or MATTER_PHYSX_ROOT.'
    }
    if (-not $CudaRoot) {
        throw '-EnablePhysx requires -CudaRoot or CUDA_PATH_V12_8.'
    }
    $configure += ' -DMATTER_ENABLE_PHYSX=ON -DMATTER_PHYSX_ROOT:PATH="{0}" -DMATTER_CUDA_ROOT:PATH="{1}"' -f $PhysxRoot, $CudaRoot
    if ($Target -eq 'matter_dist') {
        if (-not $HydrologyCache) {
            $HydrologyCache = Join-Path $repositoryRoot `
                'projects\world_demo\.cache\RiverHydrology'
        }
        if (-not (Test-Path -LiteralPath $HydrologyCache -PathType Container)) {
            throw "matter_dist with PhysX requires an accepted RiverHydrology network cache: $HydrologyCache"
        }
        $resolvedHydrologyCache = (Resolve-Path -LiteralPath $HydrologyCache).Path
        $configure += ' -DMATTER_DIST_HYDROLOGY_ARTIFACT:PATH="{0}"' -f $resolvedHydrologyCache
    }
} else {
    # Always reset the shared preset build tree so an opt-in build cannot make
    # a later ordinary editor build retain PhysX from the CMake cache.
    $configure += ' -DMATTER_ENABLE_PHYSX=OFF'
}
& $env:ComSpec /d /s /c $configure
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$build = '{0} && "{1}" --build --preset "{2}"' -f $developerEnvironment, $toolchain.CMake, $preset
if ($Target) {
    $build += ' --target "{0}"' -f $Target
}
& $env:ComSpec /d /s /c $build
$buildExitCode = $LASTEXITCODE
if ($buildExitCode -eq 0) {
    if ($Target -eq 'matter_dist') {
        $package = Join-Path $repositoryRoot 'MatterEditor\build\dist\world_demo'
        foreach ($required in @('editor.exe', 'build_features.json')) {
            if (-not (Test-Path -LiteralPath (Join-Path $package $required) -PathType Leaf)) {
                Write-Error "matter_dist succeeded but verified package file was not found: $package\$required"
                exit 1
            }
        }
        Write-Output "MATTER_WINDOWS_PACKAGE=$package"
    } elseif ((-not $Target) -or $Target -in @('matter_editor', 'editor', 'all')) {
        $artifact = Join-Path $repositoryRoot 'MatterEditor\build\windows-msvc\editor.exe'
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            $targetLabel = if ($Target) { $Target } else { '<default>' }
            Write-Error "editor-producing target '$targetLabel' succeeded but $artifact was not found"
            exit 1
        }
        Write-Output "MATTER_WINDOWS_ARTIFACT=$artifact"
    }
}
exit $buildExitCode
