param([Parameter(Mandatory = $true)][string]$Directory)
$ErrorActionPreference = 'Stop'
$child = Join-Path $PSScriptRoot 'probe_child.ps1'
$ps = Join-Path $PSHOME 'powershell.exe'
& $ps -NoProfile -ExecutionPolicy Bypass -File $child -Out (Join-Path $Directory 'wsl-console.json')
# Historical driver form (run_native_build.ps1 line 30): Start-Process + redirection, no -NoNewWindow.
$args1 = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -Out "{1}"' -f $child, (Join-Path $Directory 'historical-start-process.json')
$p1 = Start-Process -FilePath $ps -ArgumentList $args1 -PassThru `
    -RedirectStandardOutput (Join-Path $Directory 'h.out.txt') -RedirectStandardError (Join-Path $Directory 'h.err.txt')
$null = $p1.Handle
if (-not $p1.WaitForExit(60000)) { & taskkill.exe /PID $p1.Id /T /F; throw 'historical probe timeout' }
# Corrected form: CreateNoWindow, as in MatterEditor/tools/smoke_vulkan_faults.ps1.
$info = New-Object System.Diagnostics.ProcessStartInfo
$info.FileName = $ps
$info.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -Out "{1}"' -f $child, (Join-Path $Directory 'corrected-no-window.json')
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$p2 = [System.Diagnostics.Process]::Start($info)
$o = $p2.StandardOutput.ReadToEndAsync(); $e = $p2.StandardError.ReadToEndAsync()
if (-not $p2.WaitForExit(60000)) { & taskkill.exe /PID $p2.Id /T /F; throw 'corrected probe timeout' }
[ordered]@{ driver_pid = $PID; historical_exit = $p1.ExitCode; corrected_exit = $p2.ExitCode } | ConvertTo-Json
