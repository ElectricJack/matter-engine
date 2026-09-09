[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CMakeExecutable,
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$BinaryDirectory,
    [Parameter(Mandatory = $true)][string]$PhysxSdkRoot,
    [Parameter(Mandatory = $true)][string]$CudaRoot,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [Parameter(Mandatory = $true)][string]$FreeglutPath,
    [string]$WindowsSdkVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'

function ConvertTo-CMakePath([string]$Path) {
    return [IO.Path]::GetFullPath($Path).Replace('\', '/')
}

$cmakeSource = ConvertTo-CMakePath $SourceDirectory
$cmakeBinary = ConvertTo-CMakePath $BinaryDirectory
$cmakePhysx = ConvertTo-CMakePath $PhysxSdkRoot
$cmakeCuda = ConvertTo-CMakePath $CudaRoot
$cmakeOutput = ConvertTo-CMakePath $OutputRoot
$cmakeFreeglut = ConvertTo-CMakePath $FreeglutPath

# Upstream generates PxConfig.h in the source checkout even for an out-of-tree
# build. Preserve the validated control's version so Matter's static variant
# cannot contaminate later official-control builds. Static targets also receive
# PX_PHYSX_STATIC_LIB directly from upstream's generated project definitions.
$configHeader = Join-Path $PhysxSdkRoot 'include\PxConfig.h'
$hadConfigHeader = Test-Path -LiteralPath $configHeader -PathType Leaf
$originalConfigHeader = if ($hadConfigHeader) {
    [IO.File]::ReadAllBytes($configHeader)
} else {
    $null
}

try {
    $arguments = @(
        '-S', $cmakeSource,
        '-B', $cmakeBinary,
        '-G', 'Visual Studio 17 2022',
        '-A', 'x64',
        '-T', "cuda=$cmakeCuda",
        "-DPHYSX_ROOT_DIR:PATH=$cmakePhysx",
        '-DTARGET_BUILD_PLATFORM:STRING=windows',
        "-DCMAKE_SYSTEM_VERSION:STRING=$WindowsSdkVersion",
        "-DCMAKE_INSTALL_PREFIX:PATH=$cmakeOutput/install",
        "-DCUDAToolkit_ROOT:PATH=$cmakeCuda",
        "-DPX_OUTPUT_BIN_DIR:PATH=$cmakeOutput",
        "-DPX_OUTPUT_LIB_DIR:PATH=$cmakeOutput",
        "-DPHYSX_SLN_FREEGLUT_PATH:PATH=$cmakeFreeglut",
        '-DPX_BUILDSNIPPETS:BOOL=OFF',
        '-DPX_BUILDPVDRUNTIME:BOOL=OFF',
        '-DPX_GENERATE_STATIC_LIBRARIES:BOOL=ON',
        '-DPX_GENERATE_GPU_PROJECTS:BOOL=ON',
        '-DPX_GENERATE_GPU_PROJECTS_ONLY:BOOL=OFF',
        '-DPX_GENERATE_GPU_STATIC_LIBRARIES:BOOL=OFF',
        '-DNV_USE_STATIC_WINCRT:BOOL=ON',
        '-DNV_USE_DEBUG_WINCRT:BOOL=ON'
    )
    & $CMakeExecutable @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Static PhysX configuration failed with exit $LASTEXITCODE."
    }
} finally {
    if ($hadConfigHeader) {
        [IO.File]::WriteAllBytes($configHeader, $originalConfigHeader)
    } elseif (Test-Path -LiteralPath $configHeader) {
        Remove-Item -LiteralPath $configHeader -Force
    }
}

Write-Output 'MATTER_PHYSX_STATIC_CONFIGURE=PASS'
