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

function Get-LastWriteTicks {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-Item -LiteralPath $Path).LastWriteTimeUtc.Ticks
}

$testRoot = Join-Path $repositoryRoot 'MatterEditor\build\cmake\shader-rebuild-test'
$sourceDir = Join-Path $testRoot 'source'
$buildDir = Join-Path $testRoot 'native shader build with spaces and a long output directory'
if (Test-Path -LiteralPath $testRoot) {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $sourceDir -Force | Out-Null

$dependentAPath = Join-Path $sourceDir 'dependent_a.comp'
$dependentBPath = Join-Path $sourceDir 'dependent_b.comp'
$unrelatedShaderPath = Join-Path $sourceDir 'unrelated.comp'
$sharedIncludePath = Join-Path $sourceDir 'shared.glsl'
$unrelatedInput = Join-Path $sourceDir 'unrelated.txt'
$dependentShader = @'
#version 460
#extension GL_GOOGLE_include_directive : require
#include "shared.glsl"
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) buffer TransformProbe {
    mat4 transform_matrix;
    vec4 input_value;
    vec4 output_value;
} probe;
void main() {
    probe.output_value = shared_apply(probe.transform_matrix * probe.input_value);
}
'@
Set-Utf8NoBomContent -Path $dependentAPath -Value $dependentShader
Set-Utf8NoBomContent -Path $dependentBPath -Value $dependentShader
Set-Utf8NoBomContent -Path $sharedIncludePath -Value 'vec4 shared_apply(vec4 value) { return value; }'
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'MatterEngine3\shaders_vk\transform_probe.comp') `
    -Destination $unrelatedShaderPath
Set-Utf8NoBomContent -Path $unrelatedInput -Value 'unrelated-object-sentinel'

# The real editor has 59 shader outputs. With long/spaced checkout paths their
# expanded argv crossed cmd.exe's 8191-character limit: Ninja reported success
# while embedded_spirv.h was absent. Keep the fixture itself above that limit
# and verify the generated header's complete inventory, not just the exit code.
$shaderNames = @('dependent_a.comp', 'dependent_b.comp', 'unrelated.comp')
for ($shaderIndex = 0; $shaderIndex -lt 56; ++$shaderIndex) {
    $shaderName = 'inventory_padding_shader_{0:D2}.comp' -f $shaderIndex
    $shaderNames += $shaderName
    Copy-Item -LiteralPath $unrelatedShaderPath -Destination (Join-Path $sourceDir $shaderName)
}
Set-Utf8NoBomContent -Path (Join-Path $sourceDir 'shader_inventory.txt') `
    -Value (($shaderNames -join "`n") + "`n")
$legacyInputArguments = ($shaderNames | ForEach-Object {
    '"' + (Join-Path $buildDir "shaders\$_.spv") + '"'
}) -join ' '
if ($legacyInputArguments.Length -le 8191) {
    throw "Long-path regression fixture is too short: $($legacyInputArguments.Length) characters"
}
Write-Host "Legacy expanded shader inputs exceed CMD limit: $($legacyInputArguments.Length) characters"

$cmakeLists = @'
cmake_minimum_required(VERSION 3.25)
project(matter_shader_rebuild_fixture LANGUAGES NONE)

include("${MATTER_REPOSITORY_ROOT}/cmake/MatterShaders.cmake")
matter_resolve_windows_python(
    EXECUTABLE "${MATTER_PYTHON_EXECUTABLE}"
    OUT_ARGUMENTS python_arguments
    OUT_VERSION python_version)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/shader_inventory.txt" fixture_shaders)
matter_add_vulkan_shader_pipeline(
    PREFIX fixture
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    EMBEDDED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_spirv.h"
    GLSLC "${MATTER_GLSLC}"
    PYTHON_EXECUTABLE "${MATTER_PYTHON_EXECUTABLE}"
    PYTHON_ARGUMENTS ${python_arguments}
    EMBED_SCRIPT "${MATTER_REPOSITORY_ROOT}/MatterEngine3/tools/embed_spirv.py"
    SHADERS ${fixture_shaders}
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
    "-DMATTER_PYTHON_EXECUTABLE=$($toolchain.Python)"
)

Invoke-Checked -Executable $toolchain.CMake -Arguments @('--build', $buildDir, '--target', 'shader_fixture')

$dependentASpirv = Join-Path $buildDir 'shaders\dependent_a.comp.spv'
$dependentBSpirv = Join-Path $buildDir 'shaders\dependent_b.comp.spv'
$unrelatedSpirv = Join-Path $buildDir 'shaders\unrelated.comp.spv'
$headerPath = Join-Path $buildDir 'generated\embedded_spirv.h'
$unrelatedObject = Join-Path $buildDir 'unrelated.obj'
foreach ($requiredPath in @($dependentASpirv, $dependentBSpirv, $unrelatedSpirv,
        $headerPath, $unrelatedObject)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Expected shader fixture output was not created: $requiredPath"
    }
}

$headerText = Get-Content -LiteralPath $headerPath -Raw -Encoding UTF8
$lookupRows = @([regex]::Matches($headerText, 'if \(name == "'))
if ($lookupRows.Count -ne $shaderNames.Count) {
    throw "Expected $($shaderNames.Count) embedded lookups, found $($lookupRows.Count)"
}
foreach ($shaderName in $shaderNames) {
    $spirvPath = Join-Path $buildDir "shaders\$shaderName.spv"
    if (-not (Test-Path -LiteralPath $spirvPath -PathType Leaf)) {
        throw "Expected inventory shader output was not created: $spirvPath"
    }
    $escapedName = ([System.Text.Encoding]::UTF8.GetBytes("$shaderName.spv") |
        ForEach-Object { '\x{0:x2}' -f $_ }) -join ''
    if (-not $headerText.Contains('if (name == "' + $escapedName + '")')) {
        throw "Generated header omitted expected shader lookup: $shaderName.spv"
    }
}

$first = [ordered]@{
    DependentAHash = Get-Sha256 -Path $dependentASpirv
    DependentBHash = Get-Sha256 -Path $dependentBSpirv
    UnrelatedSpirvHash = Get-Sha256 -Path $unrelatedSpirv
    HeaderHash = Get-Sha256 -Path $headerPath
    UnrelatedHash = Get-Sha256 -Path $unrelatedObject
    DependentAMtime = Get-LastWriteTicks -Path $dependentASpirv
    DependentBMtime = Get-LastWriteTicks -Path $dependentBSpirv
    UnrelatedSpirvMtime = Get-LastWriteTicks -Path $unrelatedSpirv
    HeaderMtime = Get-LastWriteTicks -Path $headerPath
    UnrelatedMtime = Get-LastWriteTicks -Path $unrelatedObject
}

$noopOutput = (& $toolchain.CMake --build $buildDir --target shader_fixture 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "No-op shader rebuild failed:`n$noopOutput" }
$noop = [ordered]@{
    DependentAHash = Get-Sha256 -Path $dependentASpirv
    DependentBHash = Get-Sha256 -Path $dependentBSpirv
    UnrelatedSpirvHash = Get-Sha256 -Path $unrelatedSpirv
    HeaderHash = Get-Sha256 -Path $headerPath
    UnrelatedHash = Get-Sha256 -Path $unrelatedObject
    DependentAMtime = Get-LastWriteTicks -Path $dependentASpirv
    DependentBMtime = Get-LastWriteTicks -Path $dependentBSpirv
    UnrelatedSpirvMtime = Get-LastWriteTicks -Path $unrelatedSpirv
    HeaderMtime = Get-LastWriteTicks -Path $headerPath
    UnrelatedMtime = Get-LastWriteTicks -Path $unrelatedObject
}
foreach ($key in $first.Keys) {
    if ($first[$key] -ne $noop[$key]) {
        throw "No-op rebuild changed $key (before '$($first[$key])', after '$($noop[$key])')"
    }
}
if ($noopOutput -match 'dependent_[ab]\.comp|unrelated\.comp|embedded_spirv|unrelated\.obj|glslc(?:\.exe)?|embed_spirv\.py') {
    throw "No-op shader build executed a fixture production command:`n$noopOutput"
}

Set-Utf8NoBomContent -Path $dependentAPath -Value (
    (Get-Content -LiteralPath $dependentAPath -Raw).Replace(
        'probe.transform_matrix * probe.input_value',
        '2.0 * probe.transform_matrix * probe.input_value'))
$directOutput = (& $toolchain.CMake --build $buildDir --target shader_fixture 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "Direct shader rebuild failed:`n$directOutput" }
$direct = [ordered]@{
    DependentAHash = Get-Sha256 -Path $dependentASpirv
    DependentBHash = Get-Sha256 -Path $dependentBSpirv
    UnrelatedSpirvHash = Get-Sha256 -Path $unrelatedSpirv
    HeaderHash = Get-Sha256 -Path $headerPath
    UnrelatedHash = Get-Sha256 -Path $unrelatedObject
}
if ($direct.DependentAHash -eq $first.DependentAHash -or
        $direct.HeaderHash -eq $first.HeaderHash) {
    throw 'Direct shader edit did not change the dependent SPIR-V and embedded header hashes'
}
foreach ($stableKey in 'DependentBHash', 'UnrelatedSpirvHash', 'UnrelatedHash') {
    if ($direct[$stableKey] -ne $first[$stableKey]) {
        throw "Direct shader edit changed unrelated hash $stableKey"
    }
}
if ($directOutput -notmatch 'dependent_a\.comp' -or
        $directOutput -match 'dependent_b\.comp|unrelated\.comp|unrelated\.obj') {
    throw "Direct shader edit rebuilt an incorrect command set:`n$directOutput"
}

Set-Utf8NoBomContent -Path $sharedIncludePath -Value `
    'vec4 shared_apply(vec4 value) { return 0.5 * value; }'
$includeOutput = (& $toolchain.CMake --build $buildDir --target shader_fixture 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "Included shader rebuild failed:`n$includeOutput" }
$include = [ordered]@{
    DependentAHash = Get-Sha256 -Path $dependentASpirv
    DependentBHash = Get-Sha256 -Path $dependentBSpirv
    UnrelatedSpirvHash = Get-Sha256 -Path $unrelatedSpirv
    HeaderHash = Get-Sha256 -Path $headerPath
    UnrelatedHash = Get-Sha256 -Path $unrelatedObject
}
foreach ($changedKey in 'DependentAHash', 'DependentBHash', 'HeaderHash') {
    if ($include[$changedKey] -eq $direct[$changedKey]) {
        throw "Shared include edit did not change dependent hash $changedKey"
    }
}
foreach ($stableKey in 'UnrelatedSpirvHash', 'UnrelatedHash') {
    if ($include[$stableKey] -ne $direct[$stableKey]) {
        throw "Shared include edit changed unrelated hash $stableKey"
    }
}
if ($includeOutput -notmatch 'dependent_a\.comp' -or
        $includeOutput -notmatch 'dependent_b\.comp' -or
        $includeOutput -match 'unrelated\.comp|unrelated\.obj') {
    throw "Shared include edit rebuilt an incorrect command set:`n$includeOutput"
}

Write-Host 'shader rebuild dependency test: PASS'
