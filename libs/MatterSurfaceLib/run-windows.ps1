<#
  Build and run the native Windows build of MatterSurfaceLib.

  The exe is cross-compiled inside WSL with MinGW (the toolchain is already set
  up there and raylib is prebuilt), then launched natively on Windows. Running
  natively uses the real NVIDIA OpenGL driver instead of WSLg's emulated d3d12
  layer, so shader compiles are fast AND the on-disk program-binary cache
  (.shader_cache/) works -- the first launch compiles + caches, later launches
  restore instantly.

  Usage (from PowerShell, in this folder):
      .\run-windows.ps1            # build via WSL, then run
      .\run-windows.ps1 -NoBuild   # just run the existing exe
#>
param([switch]$NoBuild)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
Set-Location $here   # exe must run from here so shaders/ and .shader_cache/ resolve

if (-not $NoBuild) {
    $wslHere = (wsl wslpath -a "$here").Trim()
    Write-Host "Building Windows exe via WSL/MinGW in: $wslHere"
    wsl bash -lc "cd '$wslHere' && make"
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }
}

$exe = Join-Path $here "gpu_raytrace.exe"
if (-not (Test-Path $exe)) {
    throw "Executable not found: $exe`nRun without -NoBuild to build it first."
}

Write-Host "Launching $exe ..."
& $exe
