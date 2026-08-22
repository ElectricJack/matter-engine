[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,

    [Parameter(Mandatory = $true)]
    [string]$Ninja
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$executablePath = (Resolve-Path $Executable).Path
$buildPath = (Resolve-Path $BuildDirectory).Path
$ninjaPath = (Resolve-Path $Ninja).Path

Import-Module (Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1') -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot

$developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f `
    $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
$dumpCommand = '{0} && dumpbin.exe /dependents "{1}"' -f $developerEnvironment, $executablePath
$dumpOutput = (& $env:ComSpec /d /s /c $dumpCommand 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /dependents failed:`n$dumpOutput"
}

$forbiddenRuntimePattern = '(?i)libstdc\+\+|libgcc|libwinpthread'
if ($dumpOutput -match $forbiddenRuntimePattern) {
    throw "GNU runtime dependency found in smoke executable:`n$($Matches[0])"
}

$ninjaCommands = @(& $ninjaPath -C $buildPath -t commands matter_third_party_smoke 2>&1)
if ($LASTEXITCODE -ne 0) {
    throw "Ninja command-closure query failed:`n$($ninjaCommands -join [Environment]::NewLine)"
}

$archivePattern = '(?i)(?:^|[\s"])[^\s"]*\.a(?=$|[\s"])'
$toolchainContaminationPattern = '(?i)libstdc\+\+|libgcc|libwinpthread|mingw|msys|ucrt64'
$contaminatedCommands = $ninjaCommands | Where-Object {
    $_ -match $archivePattern -or $_ -match $toolchainContaminationPattern
}
if ($contaminatedCommands) {
    throw "Non-MSVC input found in smoke command closure:`n$($contaminatedCommands -join [Environment]::NewLine)"
}

$foreignArtifacts = Get-ChildItem -LiteralPath $buildPath -Recurse -File | Where-Object {
    $_.Extension -in '.a', '.o'
}
if ($foreignArtifacts) {
    throw "GNU archive/object found in MSVC build tree:`n$($foreignArtifacts.FullName -join [Environment]::NewLine)"
}

$dependencies = [regex]::Matches($dumpOutput, '(?im)^\s+[^\s]+\.dll\s*$') |
    ForEach-Object { $_.Value.Trim() }
Write-Output "matter_third_party_binary_audit: clean"
Write-Output "  dependencies: $($dependencies -join ', ')"
Write-Output "  Ninja closure: $($ninjaCommands.Count) commands; no .a/MinGW/GNU runtime inputs"
