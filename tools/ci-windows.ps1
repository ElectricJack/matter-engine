[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $repositoryRoot
$transcribing = $false
try {
    $evidence = Join-Path $repositoryRoot 'artifacts/ci'
    New-Item -ItemType Directory -Force -Path $evidence | Out-Null
    Start-Transcript -Path (Join-Path $evidence 'native-windows.log') -Force | Out-Null
    $transcribing = $true
    if (-not $env:CMAKE_BUILD_PARALLEL_LEVEL) { $env:CMAKE_BUILD_PARALLEL_LEVEL = '4' }
    if (-not $env:CTEST_PARALLEL_LEVEL) { $env:CTEST_PARALLEL_LEVEL = '2' }
    Import-Module (Join-Path $PSScriptRoot 'windows/MatterWindowsToolchain.psm1') -Force
    $toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot
    $toolchain | ConvertTo-Json -Depth 2 | Set-Content (Join-Path $evidence 'toolchain.json')
    & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config RelWithDebInfo
    if ($LASTEXITCODE -ne 0) { throw "Native build failed: $LASTEXITCODE" }
    $ctest = Join-Path (Split-Path $toolchain.CMake) 'ctest.exe'
    & $ctest --preset windows-msvc-relwithdebinfo --no-tests=error --output-junit (Join-Path $evidence 'ctest.xml')
    if ($LASTEXITCODE -ne 0) { throw "CTest failed: $LASTEXITCODE" }
    & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config RelWithDebInfo -Target matter_dist
    if ($LASTEXITCODE -ne 0) { throw "Package build failed: $LASTEXITCODE" }
    & (Join-Path $PSScriptRoot 'check-windows-msvc-package.ps1') -DistPath 'MatterEditor/build/dist/world_demo'
    if ($LASTEXITCODE -ne 0) { throw "Package validation failed: $LASTEXITCODE" }
} finally {
    if ($transcribing) { Stop-Transcript | Out-Null }
    Pop-Location
}
