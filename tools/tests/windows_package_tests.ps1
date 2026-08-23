[CmdletBinding()]
param(
    [string]$RepositoryRoot,
    [string]$EditorPath
)

$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
if (-not $EditorPath) {
    $EditorPath = Join-Path $RepositoryRoot 'MatterEditor\build\windows-msvc\editor.exe'
}
$checker = Join-Path $RepositoryRoot 'tools\check-windows-msvc-package.ps1'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("matter-package-tests-" + [guid]::NewGuid().ToString('N'))
$fakeDumpbin = Join-Path $scratch 'fake-dumpbin.ps1'

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Get-FileHashLower([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Write-Manifest([string]$DistPath, [string]$CompilerId = 'MSVC') {
    $editor = Join-Path $DistPath 'editor.exe'
    $notice = Join-Path $DistPath 'THIRD_PARTY_NOTICES.txt'
    $files = [ordered]@{}
    foreach ($path in @($editor, $notice)) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $files[[IO.Path]::GetFileName($path)] = Get-FileHashLower $path
        }
    }
    [ordered]@{
        schema_version = 1
        project = 'world_demo'
        configuration = 'RelWithDebInfo'
        source_revision = 'fixture'
        crt = 'static'
        toolchain = [ordered]@{
            compiler = [ordered]@{ id = $CompilerId; version = '19.44.35221' }
            msvc_tools = '14.44.35207'
            windows_sdk = '10.0.26100.0'
            vulkan_sdk = '1.4.357.0'
        }
        dependencies = [ordered]@{ vulkan_loader = 'Windows Vulkan loader' }
        features = [ordered]@{
            autoremesher = $true
            streamline = $false
            physx = $false
            cuda = $false
        }
        files = $files
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $DistPath 'build_features.json') -Encoding utf8
}

function New-ValidFixture([string]$Name) {
    $dist = Join-Path $scratch $Name
    $projectDir = Join-Path $dist 'projects\world_demo'
    New-Item -ItemType Directory -Force -Path $projectDir | Out-Null
    Copy-Item -LiteralPath $EditorPath -Destination (Join-Path $dist 'editor.exe')
    Set-Content -LiteralPath (Join-Path $dist 'THIRD_PARTY_NOTICES.txt') -Value 'fixture dependency notices' -Encoding utf8
    foreach ($directory in @('objects', 'scenes', 'shared-lib')) {
        Copy-Item -LiteralPath (Join-Path $RepositoryRoot "projects\world_demo\$directory") `
            -Destination $projectDir -Recurse
    }
    Write-Manifest $dist
    return $dist
}

function Invoke-Checker([string]$DistPath, [string[]]$Imports = @('KERNEL32.dll', 'USER32.dll', 'vulkan-1.dll')) {
    $savedImports = $env:MATTER_PACKAGE_TEST_IMPORTS
    try {
        $env:MATTER_PACKAGE_TEST_IMPORTS = $Imports -join ';'
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $checker `
                -DistPath $DistPath -DumpbinPath $fakeDumpbin 2>&1)
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previous
        }
        return [pscustomobject]@{ ExitCode = $exitCode; Output = $output -join "`n" }
    } finally {
        $env:MATTER_PACKAGE_TEST_IMPORTS = $savedImports
    }
}

function Assert-Rejected([string]$Name, [string]$DistPath, [string]$Pattern, [string[]]$Imports = @('KERNEL32.dll')) {
    $result = Invoke-Checker $DistPath $Imports
    Assert-True ($result.ExitCode -ne 0) "$Name unexpectedly passed"
    Assert-True ($result.Output -match $Pattern) "$Name did not report '$Pattern':`n$($result.Output)"
    Write-Output "package fixture: $Name rejected"
}

try {
    Assert-True (Test-Path -LiteralPath $EditorPath -PathType Leaf) "Fixture editor is missing: $EditorPath"
    New-Item -ItemType Directory -Force -Path $scratch | Out-Null
    @'
$imports = $env:MATTER_PACKAGE_TEST_IMPORTS -split ';' | Where-Object { $_ }
Write-Output 'Microsoft (R) COFF/PE Dumper Version fixture'
Write-Output '  Image has the following dependencies:'
$imports | ForEach-Object { Write-Output "    $_" }
exit 0
'@ | Set-Content -LiteralPath $fakeDumpbin -Encoding utf8

    $missingExe = New-ValidFixture 'missing-exe'
    Remove-Item -LiteralPath (Join-Path $missingExe 'editor.exe')
    Write-Manifest $missingExe
    Assert-Rejected 'missing executable' $missingExe 'editor\.exe.*missing|missing.*editor\.exe'

    $forbidden = New-ValidFixture 'forbidden-gnu-import'
    Assert-Rejected 'forbidden GNU import' $forbidden 'libstdc\+\+|forbidden' @('KERNEL32.dll', 'libstdc++-6.dll')

    $missingRuntime = New-ValidFixture 'missing-runtime'
    Assert-Rejected 'missing required runtime DLL' $missingRuntime 'fixture_runtime\.dll.*missing|missing.*fixture_runtime\.dll' @('KERNEL32.dll', 'fixture_runtime.dll')

    $missingNotice = New-ValidFixture 'missing-notice'
    Remove-Item -LiteralPath (Join-Path $missingNotice 'THIRD_PARTY_NOTICES.txt')
    Write-Manifest $missingNotice
    Assert-Rejected 'missing notices' $missingNotice 'notice.*missing|missing.*notice'

    $missingManifest = New-ValidFixture 'missing-manifest'
    Remove-Item -LiteralPath (Join-Path $missingManifest 'build_features.json')
    Assert-Rejected 'missing manifest' $missingManifest 'build_features\.json.*missing|missing.*build_features\.json'

    $compilerMismatch = New-ValidFixture 'compiler-mismatch'
    Write-Manifest $compilerMismatch 'GNU'
    Assert-Rejected 'compiler mismatch' $compilerMismatch 'compiler.*GNU|MSVC.*required'

    $pathOnly = New-ValidFixture 'developer-path-only'
    Set-Content -LiteralPath (Join-Path $pathOnly 'editor.exe') -Value 'not a runnable PE fixture' -Encoding ascii
    Write-Manifest $pathOnly
    Assert-Rejected 'developer-PATH-only launch' $pathOnly 'clean.PATH|launch'

    $valid = New-ValidFixture 'valid'
    $validResult = Invoke-Checker $valid
    Assert-True ($validResult.ExitCode -eq 0) "valid package failed:`n$($validResult.Output)"
    Assert-True ($validResult.Output -match 'MSVC package: PASS') "valid package omitted PASS summary"
    Write-Output 'Windows MSVC package fixtures: PASS (8/8)'
} finally {
    if (Test-Path -LiteralPath $scratch) {
        Remove-Item -LiteralPath $scratch -Recurse -Force
    }
}
