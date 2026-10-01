$ErrorActionPreference = 'Stop'
$drivers = @(Get-CimInstance Win32_Process | Where-Object {
    $_.Name -like 'python*' -and $_.CommandLine -like '*MatterEngine3/tools/drive.py*' -and
    $_.CommandLine -like '*C:/tmp/matter-terrain-compact-v1*'
})
if ($drivers.Count -ne 1) { exit 3 }
$children = @(Get-CimInstance Win32_Process -Filter "Name = 'editor.exe'" | Where-Object {
    $_.ParentProcessId -eq $drivers[0].ProcessId
})
if ($children.Count -ne 1) { exit 3 }
$editorProcess = Get-Process -Id $children[0].ProcessId
$window = $editorProcess.MainWindowHandle
if ($window -eq 0) { exit 3 }
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MatterBenchmarkWindow {
    [DllImport("user32.dll")] public static extern bool EnableWindow(IntPtr hWnd, bool enabled);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hWnd);
}
'@
[MatterBenchmarkWindow]::EnableWindow($window, $false) | Out-Null
$enabled = [MatterBenchmarkWindow]::IsWindowEnabled($window)
@{ editor_pid=$editorProcess.Id; driver_pid=$drivers[0].ProcessId; window=$window.ToInt64(); enabled=$enabled } | ConvertTo-Json -Compress
if ($enabled) { exit 4 }
