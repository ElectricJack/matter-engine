[CmdletBinding()]
param([string]$RepositoryRoot)

$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
} else {
    $RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
}

$fixtureRoot = Join-Path $RepositoryRoot 'MatterEditor\build\wrapper-native-fixture'
if (Test-Path -LiteralPath $fixtureRoot) {
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
}
New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'tools\windows') -Force |
    Out-Null
Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'tools\build-windows.ps1') `
    -Destination (Join-Path $fixtureRoot 'tools\build-windows.ps1')

$fakeCommand = Join-Path $fixtureRoot 'fake-command.cmd'
Set-Content -LiteralPath $fakeCommand -Encoding ASCII -Value '@exit /b 0'
$module = @'
function Resolve-MatterWindowsToolchain {
    param([string]$RepositoryRoot)
    $fake = Join-Path $RepositoryRoot 'fake-command.cmd'
    [pscustomobject]@{
        VsDevCmd = $fake
        WindowsSdkVersion = 'fixture-sdk'
        MsvcToolsVersion = 'fixture-msvc'
        CMake = $fake
        Ninja = $fake
        Python = $fake
    }
}
Export-ModuleMember -Function Resolve-MatterWindowsToolchain
'@
Set-Content -LiteralPath (Join-Path $fixtureRoot 'tools\windows\MatterWindowsToolchain.psm1') `
    -Encoding UTF8 -Value $module

$artifact = Join-Path $fixtureRoot 'MatterEditor\build\windows-msvc\editor.exe'
function Invoke-Fixture([string]$Target, [bool]$CreateArtifact) {
    if (Test-Path -LiteralPath $artifact) {
        Remove-Item -LiteralPath $artifact -Force
    }
    if ($CreateArtifact) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $artifact) -Force |
            Out-Null
        Set-Content -LiteralPath $artifact -Encoding ASCII -Value 'fixture'
    }
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', (Join-Path $fixtureRoot 'tools\build-windows.ps1'),
        '-Config', 'RelWithDebInfo'
    )
    if ($Target) { $arguments += @('-Target', $Target) }
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & powershell.exe @arguments 2>&1 | Out-String
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    [pscustomobject]@{ ExitCode = $exitCode; Output = $output }
}

$nonEditor = Invoke-Fixture 'compiler_portability_tests' $true
if ($nonEditor.ExitCode -ne 0 -or
        $nonEditor.Output -match 'MATTER_WINDOWS_ARTIFACT=') {
    throw "non-editor target emitted an editor artifact marker`n$($nonEditor.Output)"
}

$missing = Invoke-Fixture 'matter_editor' $false
if ($missing.ExitCode -eq 0 -or
        $missing.Output -match 'MATTER_WINDOWS_ARTIFACT=') {
    throw "missing editor artifact was not rejected`n$($missing.Output)"
}

$editor = Invoke-Fixture 'matter_editor' $true
$editorMarkers = @($editor.Output -split "`r?`n" |
    Where-Object { $_ -match '^MATTER_WINDOWS_ARTIFACT=' })
if ($editor.ExitCode -ne 0 -or $editorMarkers.Count -ne 1) {
    throw "editor target did not emit exactly one verified marker`n$($editor.Output)"
}

$default = Invoke-Fixture '' $true
$defaultMarkers = @($default.Output -split "`r?`n" |
    Where-Object { $_ -match '^MATTER_WINDOWS_ARTIFACT=' })
if ($default.ExitCode -ne 0 -or $defaultMarkers.Count -ne 1) {
    throw "default build did not emit exactly one verified marker`n$($default.Output)"
}

$repositoryPath = [System.IO.Path]::GetFullPath($RepositoryRoot)
if ($repositoryPath.Length -lt 3 -or $repositoryPath[1] -ne ':') {
    throw "WSL wrapper fixture requires a drive-qualified repository path: $repositoryPath"
}
$drive = [char]::ToLowerInvariant($repositoryPath[0])
$relative = $repositoryPath.Substring(3).Replace('\', '/')
$wslRoot = "/mnt/$drive/$relative"
$wslScript = "$wslRoot/cmake/tests/build_windows_from_wsl_contract_tests.sh"
& wsl.exe -- env -i PATH=/usr/bin:/bin HOME=/tmp bash $wslScript $wslRoot
if ($LASTEXITCODE -ne 0) {
    throw "WSL build-wrapper contract failed with exit $LASTEXITCODE"
}

Write-Host 'native and WSL build-wrapper contracts passed'
