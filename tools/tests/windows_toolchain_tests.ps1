$ErrorActionPreference = 'Stop'

function Fail-Test {
    param([Parameter(Mandatory = $true)][string]$Message)

    [Console]::Error.WriteLine($Message)
    exit 1
}

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        Fail-Test $Message
    }
}

function Assert-CommandSucceeds {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Name
    )

    & $Path @Arguments
    if ($LASTEXITCODE -ne 0) {
        Fail-Test "$Name probe failed with exit code ${LASTEXITCODE}: $Path $($Arguments -join ' ')"
    }
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$modulePath = Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1'

if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
    Fail-Test 'MatterWindowsToolchain.psm1 was not found'
}

Import-Module $modulePath -Force

$dependencyChecker = Join-Path $repositoryRoot 'tools\check-windows-msvc-deps.ps1'
Assert-CommandSucceeds -Path (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $dependencyChecker) -Name 'Windows dependency checker'

try {
    Resolve-MatterWindowsToolchain -RepositoryRoot (Join-Path $repositoryRoot 'does-not-exist') | Out-Null
    Fail-Test 'Resolve-MatterWindowsToolchain accepted a missing repository'
}
catch {
    if ($_.Exception.Message -notmatch 'Repository root') {
        Fail-Test "Missing repository was rejected for the wrong reason: $($_.Exception.Message)"
    }
}

$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot

Assert-True ($toolchain.VisualStudioRoot -match '[\\/]2022[\\/]') 'Visual Studio 2022 was not selected'
Assert-True ($toolchain.VisualStudioRoot -notmatch '[\\/]18([\\/]|$)') 'Visual Studio 18 must not be selected'
Assert-True ($toolchain.MsvcToolsVersion -eq '14.44.35207') "Expected MSVC 14.44.35207, got $($toolchain.MsvcToolsVersion)"
Assert-True ($toolchain.WindowsSdkVersion -eq '10.0.26100.0') "Expected Windows SDK 10.0.26100.0, got $($toolchain.WindowsSdkVersion)"
Assert-True ($toolchain.CMake.StartsWith($toolchain.VisualStudioRoot, [System.StringComparison]::OrdinalIgnoreCase)) 'CMake was not selected from Visual Studio'
Assert-True ($toolchain.Ninja.StartsWith($toolchain.VisualStudioRoot, [System.StringComparison]::OrdinalIgnoreCase)) 'Ninja was not selected from Visual Studio'
Assert-True ($toolchain.VulkanSdk -eq 'C:\VulkanSDK\1.4.357.0') "Expected Vulkan SDK 1.4.357.0, got $($toolchain.VulkanSdk)"
Assert-True ($toolchain.Glslc -eq 'C:\VulkanSDK\1.4.357.0\Bin\glslc.exe') "Expected Vulkan glslc.exe, got $($toolchain.Glslc)"

Assert-CommandSucceeds -Path $toolchain.CMake -Arguments @('--version') -Name 'CMake'
Assert-CommandSucceeds -Path $toolchain.Ninja -Arguments @('--version') -Name 'Ninja'
Assert-CommandSucceeds -Path $toolchain.Python -Arguments @('--version') -Name 'Python'
Assert-CommandSucceeds -Path $toolchain.Glslc -Arguments @('--version') -Name 'Vulkan glslc'

Write-Output 'windows_toolchain_tests: PASS'
