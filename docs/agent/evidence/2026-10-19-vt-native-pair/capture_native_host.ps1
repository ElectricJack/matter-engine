$ErrorActionPreference = 'Stop'
[ordered]@{
    captured_at = (Get-Date).ToUniversalTime().ToString('o')
    powershell_version = $PSVersionTable.PSVersion.ToString()
    os = [Environment]::OSVersion.VersionString
    gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, DriverDate)
    relevant_processes = @(Get-CimInstance Win32_Process | Where-Object {
        $_.Name -match '^(editor|vulkan_smoke_tests|ctest|ninja|cl|glslc|cmake)\.exe$'
    } | Select-Object Name, ProcessId, ExecutablePath)
} | ConvertTo-Json -Depth 4
