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
Set-Content -LiteralPath $fakeCommand -Encoding ASCII -Value @('@echo %*>>"%~dp0commands.log"', '@exit /b 0')
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
$package = Join-Path $fixtureRoot 'MatterEditor\build\dist\world_demo'
function Invoke-Fixture([string]$Target, [bool]$CreateArtifact, [bool]$CreatePackage = $false,
        [string]$BuildDirectory, [string]$CompilerLauncher, [switch]$ConfigureOnly) {
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'commands.log') -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $artifact) {
        Remove-Item -LiteralPath $artifact -Force
    }
    if ($CreateArtifact) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $artifact) -Force |
            Out-Null
        Set-Content -LiteralPath $artifact -Encoding ASCII -Value 'fixture'
    }
    if (Test-Path -LiteralPath $package) {
        Remove-Item -LiteralPath $package -Recurse -Force
    }
    if ($CreatePackage) {
        New-Item -ItemType Directory -Path $package -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $package 'editor.exe') -Encoding ASCII -Value 'fixture'
        Set-Content -LiteralPath (Join-Path $package 'build_features.json') -Encoding ASCII -Value '{}'
    }
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', (Join-Path $fixtureRoot 'tools\build-windows.ps1'),
        '-Config', 'RelWithDebInfo'
    )
    if ($Target) { $arguments += @('-Target', $Target) }
    if ($BuildDirectory) { $arguments += @('-BuildDirectory', $BuildDirectory) }
    if ($CompilerLauncher) { $arguments += @('-CompilerLauncher', $CompilerLauncher) }
    if ($ConfigureOnly) { $arguments += '-ConfigureOnly' }
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

$missingPackage = Invoke-Fixture 'matter_dist' $false $false
if ($missingPackage.ExitCode -eq 0 -or $missingPackage.Output -match 'MATTER_WINDOWS_PACKAGE=') {
    throw "missing package was not rejected`n$($missingPackage.Output)"
}

$dist = Invoke-Fixture 'matter_dist' $false $true
$packageMarkers = @($dist.Output -split "`r?`n" |
    Where-Object { $_ -match '^MATTER_WINDOWS_PACKAGE=' })
if ($dist.ExitCode -ne 0 -or $packageMarkers.Count -ne 1 -or
        $dist.Output -match 'MATTER_WINDOWS_ARTIFACT=') {
    throw "matter_dist did not emit exactly one verified package marker`n$($dist.Output)"
}

$external = Join-Path $fixtureRoot 'external build'
$cached = Invoke-Fixture -Target compiler_portability_tests -CreateArtifact $false `
    -BuildDirectory $external -CompilerLauncher $fakeCommand
$commands = Get-Content -Raw (Join-Path $fixtureRoot 'commands.log')
if ($cached.ExitCode -ne 0 -or $commands -notlike "*-B `"$external`"*" -or
        $commands -notlike "*--build `"$external`"*" -or
        $commands -notmatch 'CMAKE_MSVC_DEBUG_INFORMATION_FORMAT:STRING=Embedded' -or
        $commands -notmatch 'CMAKE_CXX_COMPILER_LAUNCHER:FILEPATH=') {
    throw "external cached build arguments were incorrect`n$commands`n$($cached.Output)"
}
$configureOnly = Invoke-Fixture -Target matter_editor -CreateArtifact $false `
    -BuildDirectory $external -ConfigureOnly
$commands = Get-Content -Raw (Join-Path $fixtureRoot 'commands.log')
if ($configureOnly.ExitCode -ne 0 -or $commands -match '--build' -or
        $commands -notmatch 'CMAKE_C_COMPILER_LAUNCHER:FILEPATH= ' -or
        $commands -notmatch 'CMAKE_CXX_COMPILER_LAUNCHER:FILEPATH= ' -or
        $commands -notmatch 'ProgramDatabase') {
    throw "configure-only did not clear launchers or tried to build`n$commands`n$($configureOnly.Output)"
}

$repositoryPath = [System.IO.Path]::GetFullPath($RepositoryRoot)
if ($repositoryPath.Length -lt 3 -or $repositoryPath[1] -ne ':') {
    throw "WSL wrapper fixture requires a drive-qualified repository path: $repositoryPath"
}
$drive = [char]::ToLowerInvariant($repositoryPath[0])
$relative = $repositoryPath.Substring(3).Replace('\', '/')
$wslRoot = "/mnt/$drive/$relative"
$wslScript = "$wslRoot/cmake/tests/build_windows_from_wsl_contract_tests.sh"
# wsl.exe itself exits -1 when the WSL service cannot start the command at
# all (the self-hosted runner shares its WSL VM with the agent fleet, and a
# saturated VM answers Wsl/Service/0x8007274c, a connection timeout). bash can
# only exit 0-255, so -1 never reports a contract result: retry just that
# launch failure, and let every exit the script produced stand.
$wslAttempts = 4
for ($attempt = 1; $attempt -le $wslAttempts; ++$attempt) {
    & wsl.exe -- env -i PATH=/usr/bin:/bin HOME=/tmp bash $wslScript $wslRoot
    $wslExit = $LASTEXITCODE
    if ($wslExit -ne -1) { break }
    Write-Host "WSL could not launch the contract (attempt $attempt of $wslAttempts)"
    if ($attempt -lt $wslAttempts) { Start-Sleep -Seconds 30 }
}
if ($wslExit -ne 0) {
    throw "WSL build-wrapper contract failed with exit $wslExit"
}

Write-Host 'native and WSL build-wrapper contracts passed'
