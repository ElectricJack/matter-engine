[CmdletBinding()]
param(
    [string]$FixtureScript
)

# Exercise the actual positive-fixture calls with an in-memory filesystem. Any
# attempt to read/copy the checkout (including an arbitrarily large live cache)
# fails immediately. No compiler, PE checker, temp tree or editor is involved.
$ErrorActionPreference = 'Stop'
if (-not $FixtureScript) { $FixtureScript = Join-Path $PSScriptRoot 'windows_package_tests.ps1' }
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($FixtureScript, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$fixtureFunction = $ast.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'New-ValidFixture'
}, $true)
if (-not $fixtureFunction) { throw 'New-ValidFixture is missing' }
Invoke-Expression $fixtureFunction.Extent.Text

$scratch = 'C:\in-memory-package-tests'
$RepositoryRoot = 'C:\unreadable-live-checkout'
$expectedNotices = @('fixture-notice')
$nvidiaNotices = @('nvidia_physx', 'nvidia_cuda')
$streamlineNotices = @('nvidia_streamline', 'nvidia_dlss')
$streamlineRuntime = @('sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll', 'nvngx_dlss.dll')
$script:peFixture = [pscustomobject]@{
    Editor = 'C:\in-memory-binaries\editor.exe'
    StandaloneEditor = 'C:\in-memory-binaries\standalone.exe'
    RuntimeDll = 'C:\in-memory-binaries\fixture_runtime.dll'
}
$files = @{}
$manifests = @{}
$allowedCopies = @($peFixture.Editor, $peFixture.StandaloneEditor, $peFixture.RuntimeDll)

function New-Item { param($ItemType, [switch]$Force, $Path) }
function Copy-Item {
    param($LiteralPath, $Destination, [switch]$Recurse)
    if ($Recurse -or $LiteralPath -notin $allowedCopies) {
        throw "fixture accessed live content: $LiteralPath (recurse=$Recurse)"
    }
    $files[$Destination] = "binary:$LiteralPath"
}
function Set-Content {
    param($LiteralPath, $Value, $Encoding)
    $files[$LiteralPath] = $Value
}
function Test-Path { param($LiteralPath); throw "fixture inspected live content: $LiteralPath" }
function Remove-Item { param($LiteralPath, [switch]$Recurse, [switch]$Force); throw 'fixture attempted cleanup of live content' }
function Write-Manifest {
    param($DistPath, $Configuration, $RuntimeDlls, $Physx, $Cuda, $Streamline, $Notices)
    $manifests[$DistPath] = @($RuntimeDlls)
}

# Run the suite's own positive call sites, so reintroducing -CompleteContent or
# another source-dependent mode cannot evade this regression by testing a
# different fixture configuration.
foreach ($variable in @('valid', 'validPhysx')) {
    $assignment = $ast.Find({
        param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
            $node.Left.VariablePath.UserPath -eq $variable
    }, $true)
    if (-not $assignment) { throw "positive fixture call is missing: $variable" }
    Invoke-Expression $assignment.Extent.Text
    $dist = Get-Variable -Name $variable -ValueOnly
    foreach ($relative in @('editor.exe', 'editor.pdb', 'THIRD_PARTY_NOTICES.txt',
            'projects\world_demo\fixture-scene.js', 'MatterEngine3\shared-lib\fixture-runtime.js')) {
        if (-not $files.ContainsKey((Join-Path $dist $relative))) { throw "missing fixture content: $relative" }
    }
    if (-not $manifests.ContainsKey($dist)) { throw 'fixture manifest was not constructed' }
    if ($variable -eq 'validPhysx') {
        foreach ($relative in @('PhysXGpu_64.dll', 'licenses\NVIDIA_PhysX_LICENSE.md', 'licenses\NVIDIA_CUDA_EULA.txt',
                'projects\world_demo\.cache\RiverHydrology\hydrology\network.mhyn',
                'projects\world_demo\.cache\RiverHydrology\hydrology\sections\upper.mhyd',
                'projects\world_demo\.cache\RiverHydrology\hydrology\sections\lower.mhyd',
                'projects\world_demo\.cache\RiverHydrology\hydrology\handoffs\pool-one.mhyd')) {
            if (-not $files.ContainsKey((Join-Path $dist $relative))) { throw "missing PhysX fixture content: $relative" }
        }
        if (($manifests[$dist] -join ';') -ne 'PhysXGpu_64.dll') { throw 'incorrect PhysX runtime manifest' }
    } elseif (($manifests[$dist] -join ';') -ne 'fixture_runtime.dll') { throw 'incorrect runtime manifest' }
}
Write-Output 'Windows package fixture isolation: PASS (positive fixtures use only in-memory content and fixture binaries)'
