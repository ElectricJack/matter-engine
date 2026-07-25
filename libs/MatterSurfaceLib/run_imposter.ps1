<#
  run_imposter.ps1 - launch the interactive imposter demo cleanly.

  Usage (from the MatterSurfaceLib folder):
    .\run_imposter.ps1            # fitted part imposter
    .\run_imposter.ps1 cube       # cube imposter (simplest case)
    .\run_imposter.ps1 cube 1     # cube + MSL_IMP_DBG=1 (hue per cage triangle)
    .\run_imposter.ps1 fitted 2   # fitted + MSL_IMP_DBG=2 (entry UV, skip march)
    .\run_imposter.ps1 cube -Cone 80   # override chart cone half-angle

  Always wipes every lingering MSL_* env var first so a stray MSL_CAPTURE
  (headless screenshot mode) can never sneak in again.
#>
param(
    [string]$Mode = "fitted",   # "cube" or "fitted"
    [int]$Dbg = 0,              # 0 = normal, 1 = hue per cage tri, 2 = entry UV only
    [float]$Cone = 0            # >0 overrides chart normal-cone half-angle
)

# 1. Nuke every stale MSL_* variable in this session.
Get-ChildItem Env: | Where-Object { $_.Name -like 'MSL_*' } |
    ForEach-Object { Remove-Item "Env:\$($_.Name)" }

# 2. Always show the imposter demo.
$env:MSL_SHOW_IMPOSTER = "1"

# 3. Cube vs fitted.
if ($Mode -eq "cube") { $env:MSL_IMPOSTER_CUBE = "1" }

# 4. Optional debug visualizer.
if ($Dbg -ne 0) { $env:MSL_IMP_DBG = "$Dbg" }

# 5. Optional chart-cone override.
if ($Cone -gt 0) { $env:MSL_IMP_CONE = "$Cone" }

Write-Host "Launching gpu_raytrace.exe  (Mode=$Mode  Dbg=$Dbg  Cone=$Cone)" -ForegroundColor Cyan
# Tee console output to run_imposter.log so the run can be inspected afterward.
& "$PSScriptRoot\gpu_raytrace.exe" *>&1 | Tee-Object "$PSScriptRoot\run_imposter.log"
