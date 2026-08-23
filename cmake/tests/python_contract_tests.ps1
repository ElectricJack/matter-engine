[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$toolchainModule = Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1'
Import-Module $toolchainModule -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot

$buildWrapper = Get-Content -LiteralPath (Join-Path $repositoryRoot 'tools\build-windows.ps1') -Raw
if ($buildWrapper -notmatch 'MATTER_PYTHON_EXECUTABLE:FILEPATH') {
    throw 'build-windows.ps1 does not pass the exact discovered Python executable into CMake'
}

function Invoke-Checked {
    param([string]$Executable, [string[]]$Arguments)
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $Executable $($Arguments -join ' ')"
    }
}

function Assert-PythonSelection {
    param(
        [string]$Name,
        [string]$Python,
        [bool]$ExpectLauncher
    )
    $build = Join-Path $testRoot $Name
    Invoke-Checked -Executable $toolchain.CMake -Arguments @(
        '-S', $fixtureSource,
        '-B', $build,
        '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$($toolchain.Ninja)",
        "-DMATTER_REPOSITORY_ROOT=$repositoryRoot",
        "-DMATTER_GLSLC=$($toolchain.Glslc)",
        "-DMATTER_PYTHON_EXECUTABLE:FILEPATH=$Python"
    )
    $cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Raw
    $ninjaFile = Get-Content -LiteralPath (Join-Path $build 'build.ninja') -Raw
    $normalized = $Python.Replace('\', '/')
    $escapedForNinja = $normalized.Replace(' ', '$ ')
    if ($cache -notmatch [regex]::Escape("MATTER_PYTHON_EXECUTABLE:FILEPATH=$normalized") -and
        $cache -notmatch [regex]::Escape("MATTER_PYTHON_EXECUTABLE:FILEPATH=$Python")) {
        throw "$Name did not preserve the selected executable in CMakeCache.txt"
    }
    if ($ninjaFile -notmatch [regex]::Escape($escapedForNinja) -and
        $ninjaFile -notmatch [regex]::Escape($normalized) -and
        $ninjaFile -notmatch [regex]::Escape($Python)) {
        throw "$Name did not use the selected executable in the generated embedding command"
    }
    $selectedLines = ($ninjaFile -split "`n") | Where-Object { $_ -match 'embed_spirv\.py' }
    $selectedCommand = $selectedLines -join "`n"
    if ($ExpectLauncher -and $selectedCommand -notmatch '(?:^|\s)-3(?:\s|$)') {
        throw "$Name did not apply -3 to the selected py.exe launcher"
    }
    if (-not $ExpectLauncher -and $selectedCommand -match '(?:^|\s)-3(?:\s|$)') {
        throw "$Name incorrectly applied -3 to python.exe"
    }
}

$testRoot = Join-Path $repositoryRoot 'MatterEditor\build\cmake\python-contract-test'
if (Test-Path -LiteralPath $testRoot) {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
$fixtureSource = Join-Path $testRoot 'source'
New-Item -ItemType Directory -Path $fixtureSource -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'MatterEngine3\shaders_vk\transform_probe.comp') `
    -Destination (Join-Path $fixtureSource 'fixture.comp')
$fixtureCMake = @'
cmake_minimum_required(VERSION 3.25)
project(python_contract_fixture LANGUAGES NONE)
include("${MATTER_REPOSITORY_ROOT}/cmake/MatterShaders.cmake")
matter_resolve_windows_python(
    EXECUTABLE "${MATTER_PYTHON_EXECUTABLE}"
    OUT_ARGUMENTS python_arguments
    OUT_VERSION python_version)
if(NOT python_version STREQUAL "Python 3.13.14")
    message(FATAL_ERROR "unexpected Python: ${python_version}")
endif()
matter_add_vulkan_shader_pipeline(
    PREFIX python_contract
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    EMBEDDED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_spirv.h"
    GLSLC "${MATTER_GLSLC}"
    PYTHON_EXECUTABLE "${MATTER_PYTHON_EXECUTABLE}"
    PYTHON_ARGUMENTS ${python_arguments}
    EMBED_SCRIPT "${MATTER_REPOSITORY_ROOT}/MatterEngine3/tools/embed_spirv.py"
    SHADERS fixture.comp)
'@
[System.IO.File]::WriteAllText(
    (Join-Path $fixtureSource 'CMakeLists.txt'),
    $fixtureCMake,
    [System.Text.UTF8Encoding]::new($false))
$alternateLauncherDirectory = Join-Path $testRoot 'alternate launcher with spaces'
New-Item -ItemType Directory -Path $alternateLauncherDirectory -Force | Out-Null
$alternateLauncher = Join-Path $alternateLauncherDirectory 'py.exe'
Copy-Item -LiteralPath $toolchain.Python -Destination $alternateLauncher

$nativePython = (& $toolchain.Python -3 -c 'import sys; print(sys.executable)' 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $nativePython -PathType Leaf)) {
    throw "Unable to discover python.exe through $($toolchain.Python): $nativePython"
}

Assert-PythonSelection -Name 'alternate-launcher' -Python $alternateLauncher -ExpectLauncher $true
Assert-PythonSelection -Name 'direct-python' -Python $nativePython -ExpectLauncher $false

Write-Output 'python executable contract tests: PASS'
