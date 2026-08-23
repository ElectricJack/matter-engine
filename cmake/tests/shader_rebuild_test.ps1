[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$toolchainModule = Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1'
Import-Module $toolchainModule -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot

function Set-Utf8NoBomContent {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Value
    )
    [System.IO.File]::WriteAllText($Path, $Value, [System.Text.UTF8Encoding]::new($false))
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        return ([System.BitConverter]::ToString($sha256.ComputeHash($stream))).Replace('-', '')
    }
    finally {
        $stream.Dispose()
        $sha256.Dispose()
    }
}

$testRoot = Join-Path $repositoryRoot 'MatterEditor\build\cmake\shader-rebuild-test'
$sourceDir = Join-Path $testRoot 'source'
$buildDir = Join-Path $testRoot 'build'
if (Test-Path -LiteralPath $testRoot) {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $sourceDir -Force | Out-Null

$shaderPath = Join-Path $sourceDir 'fixture.comp'
$unrelatedInput = Join-Path $sourceDir 'unrelated.txt'
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'MatterEngine3\shaders_vk\transform_probe.comp') `
    -Destination $shaderPath
Set-Utf8NoBomContent -Path $unrelatedInput -Value 'unrelated-object-sentinel'

$cmakeLists = @'
cmake_minimum_required(VERSION 3.25)
project(matter_shader_rebuild_fixture LANGUAGES NONE)

include("${MATTER_REPOSITORY_ROOT}/cmake/MatterShaders.cmake")

matter_add_vulkan_shader_pipeline(
    PREFIX fixture
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    EMBEDDED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_spirv.h"
    GLSLC "${MATTER_GLSLC}"
    PYTHON_LAUNCHER "${MATTER_PYTHON_LAUNCHER}"
    EMBED_SCRIPT "${MATTER_REPOSITORY_ROOT}/MatterEngine3/tools/embed_spirv.py"
    SHADERS fixture.comp
)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/unrelated.obj"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/unrelated.txt"
        "${CMAKE_CURRENT_BINARY_DIR}/unrelated.obj"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unrelated.txt"
    VERBATIM
)
add_custom_target(shader_fixture ALL
    DEPENDS fixture_embedded_spirv "${CMAKE_CURRENT_BINARY_DIR}/unrelated.obj")
'@
Set-Utf8NoBomContent -Path (Join-Path $sourceDir 'CMakeLists.txt') -Value $cmakeLists

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $Executable $($Arguments -join ' ')"
    }
}

Invoke-Checked -Executable $toolchain.CMake -Arguments @(
    '-S', $sourceDir,
    '-B', $buildDir,
    '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$($toolchain.Ninja)",
    "-DMATTER_REPOSITORY_ROOT=$repositoryRoot",
    "-DMATTER_GLSLC=$($toolchain.Glslc)",
    "-DMATTER_PYTHON_LAUNCHER=$($toolchain.Python)"
)

Invoke-Checked -Executable $toolchain.CMake -Arguments @('--build', $buildDir, '--target', 'shader_fixture')

$spirvPath = Join-Path $buildDir 'shaders\fixture.comp.spv'
$headerPath = Join-Path $buildDir 'generated\embedded_spirv.h'
$unrelatedObject = Join-Path $buildDir 'unrelated.obj'
foreach ($requiredPath in @($spirvPath, $headerPath, $unrelatedObject)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Expected shader fixture output was not created: $requiredPath"
    }
}

$first = [ordered]@{
    SpirvHash = Get-Sha256 -Path $spirvPath
    SpirvTime = (Get-Item -LiteralPath $spirvPath).LastWriteTimeUtc
    HeaderHash = Get-Sha256 -Path $headerPath
    HeaderTime = (Get-Item -LiteralPath $headerPath).LastWriteTimeUtc
    UnrelatedHash = Get-Sha256 -Path $unrelatedObject
    UnrelatedTime = (Get-Item -LiteralPath $unrelatedObject).LastWriteTimeUtc
}

Start-Sleep -Milliseconds 1100
Invoke-Checked -Executable $toolchain.CMake -Arguments @('--build', $buildDir, '--target', 'shader_fixture')
$noop = [ordered]@{
    SpirvHash = Get-Sha256 -Path $spirvPath
    SpirvTime = (Get-Item -LiteralPath $spirvPath).LastWriteTimeUtc
    HeaderHash = Get-Sha256 -Path $headerPath
    HeaderTime = (Get-Item -LiteralPath $headerPath).LastWriteTimeUtc
    UnrelatedHash = Get-Sha256 -Path $unrelatedObject
    UnrelatedTime = (Get-Item -LiteralPath $unrelatedObject).LastWriteTimeUtc
}
foreach ($key in $first.Keys) {
    if ($first[$key] -ne $noop[$key]) {
        throw "No-op rebuild changed $key (before '$($first[$key])', after '$($noop[$key])')"
    }
}

Set-Utf8NoBomContent -Path $shaderPath -Value (
    (Get-Content -LiteralPath $shaderPath -Raw).Replace(
        'probe.transform_matrix * probe.input_value',
        '2.0 * probe.transform_matrix * probe.input_value'))
Start-Sleep -Milliseconds 1100
Invoke-Checked -Executable $toolchain.CMake -Arguments @('--build', $buildDir, '--target', 'shader_fixture')

$secondSpirvHash = Get-Sha256 -Path $spirvPath
$secondSpirvTime = (Get-Item -LiteralPath $spirvPath).LastWriteTimeUtc
$secondHeaderHash = Get-Sha256 -Path $headerPath
$secondHeaderTime = (Get-Item -LiteralPath $headerPath).LastWriteTimeUtc
$secondUnrelatedHash = Get-Sha256 -Path $unrelatedObject
$secondUnrelatedTime = (Get-Item -LiteralPath $unrelatedObject).LastWriteTimeUtc

if ($secondSpirvHash -eq $first.SpirvHash -or $secondSpirvTime -le $first.SpirvTime) {
    throw 'Shader edit did not change both the SPIR-V hash and timestamp'
}
if ($secondHeaderHash -eq $first.HeaderHash -or $secondHeaderTime -le $first.HeaderTime) {
    throw 'Shader edit did not change both the embedded header hash and timestamp'
}
if ($secondUnrelatedHash -ne $first.UnrelatedHash -or $secondUnrelatedTime -ne $first.UnrelatedTime) {
    throw 'Shader edit rebuilt the unrelated object'
}

Write-Host 'shader rebuild dependency test: PASS'
