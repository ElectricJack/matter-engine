[CmdletBinding()]
param(
    # Comma-separated subset of static,duplicates,configure,build.
    [string]$Checks = 'static,duplicates,configure,build',
    # Restrict to these components (default: every component in rules.json).
    [string[]]$Component = @(),
    # Write the JSON report here.
    [string]$Report,
    # Delete each probe build directory first.
    [switch]$Fresh,
    # Exit 1 when anything is UNTESTED (scaffold/absent components included).
    [switch]$StrictUntested,
    # The supported toolchain installs the Vulkan SDK, so the graphics Vulkan
    # variant is required here; pass this only to diagnose a broken SDK.
    [switch]$AllowMissingVulkan,
    # Print the per-check notes (targets, commands, audited exceptions).
    [switch]$ShowNotes,
    # Run the fixture tests (tests/component-separation/test_dependency_checks.py)
    # with this toolchain instead of checking the repository.
    [switch]$SelfTest
)

# Runs check_dependencies.py with the supported MSVC toolchain: the same
# resolver and VsDevCmd environment as tools/build-windows.ps1, the bundled
# CMake and Ninja, native Python 3.13, and one compile job per probe.
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Import-Module (Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1') -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot

$python = '"{0}"' -f $toolchain.Python
if ([System.IO.Path]::GetFileName($toolchain.Python) -ieq 'py.exe') {
    $python += ' -3.13'
}
$arguments = @(
    ('"{0}"' -f (Join-Path $PSScriptRoot 'check_dependencies.py')),
    '--checks', $Checks,
    '--cmake', ('"{0}"' -f $toolchain.CMake),
    '--generator', 'Ninja',
    '--make-program', ('"{0}"' -f $toolchain.Ninja),
    '--jobs', '1'
)
foreach ($name in $Component) {
    $arguments += @('--component', $name)
}
if ($Report) {
    $arguments += @('--report', ('"{0}"' -f [System.IO.Path]::GetFullPath($Report)))
}
if ($Fresh) { $arguments += '--fresh' }
if ($StrictUntested) { $arguments += '--strict-untested' }
if ($ShowNotes) { $arguments += '--verbose' }
$probesConfigure = ($Checks -split ',') | Where-Object { $_.Trim() -in @('configure', 'build') }
if ($probesConfigure -and -not $AllowMissingVulkan) {
    $arguments += @('--require-capability', 'vulkan-sdk')
}

$developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
if ($SelfTest) {
    $runner = '"{0}"' -f (Join-Path $repositoryRoot 'tests\component-separation\test_dependency_checks.py')
    $probeToolchain = 'set "MATTER_DEPCHECK_CMAKE={0}" && set "MATTER_DEPCHECK_MAKE_PROGRAM={1}" && set "MATTER_DEPCHECK_GENERATOR=Ninja"' -f $toolchain.CMake, $toolchain.Ninja
    $command = '{0} && {1} && {2} {3} -v' -f $developerEnvironment, $probeToolchain, $python, $runner
} else {
    $command = '{0} && set "VULKAN_SDK={1}" && {2} {3}' -f $developerEnvironment, $toolchain.VulkanSdk, $python, ($arguments -join ' ')
}
& $env:ComSpec /d /s /c $command
exit $LASTEXITCODE
