$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Import-Module (Join-Path $repo 'tools/windows/MatterWindowsToolchain.psm1') -Force
$tc = Resolve-MatterWindowsToolchain -RepositoryRoot $repo
$os = Get-CimInstance Win32_OperatingSystem
$system = Get-CimInstance Win32_ComputerSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors
$video = Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, DriverDate
[ordered]@{
    captured_utc = (Get-Date).ToUniversalTime().ToString('o')
    os = $os.Caption
    os_version = $os.Version
    os_build = $os.BuildNumber
    system_model = $system.Model
    memory_bytes = $system.TotalPhysicalMemory
    cpu = $cpu
    video = $video
    toolchain = $tc
    note = 'Current CPU/native captures only. This machine snapshot does not identify the historical render campaign CPU.'
} | ConvertTo-Json -Depth 5
