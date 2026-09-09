[CmdletBinding()]
param(
    [string]$RepositoryRoot,
    [string]$EditorPath
)

$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
$checker = Join-Path $RepositoryRoot 'tools\check-windows-msvc-package.ps1'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("matter-package-tests-" + [guid]::NewGuid().ToString('N'))
$fakeDumpbin = Join-Path $scratch 'fake-dumpbin.ps1'
$fixtureBin = Join-Path $scratch 'fixture-bin'
$developerDllDirectory = Join-Path $scratch 'developer-dll'
$pinnedCompilerVersion = '19.44.35211'
$pinnedToolsVersion = '14.44.35207'
$pinnedWindowsSdk = '10.0.26100.0'
$pinnedVulkanSdk = '1.4.357.0'
$cleanDiffSha = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
Import-Module (Join-Path $RepositoryRoot 'tools\windows\MatterWindowsToolchain.psm1') -Force

$expectedDependencies = [ordered]@{
    autoremesher_core = [ordered]@{
        source = 'third_party/autoremesher_core'; languages = @('C', 'CXX')
        defines = @('_USE_MATH_DEFINES', 'GEOGRAM_WITH_PDEL', 'AUTOREMESHER_FORCE_SHIM', 'STB_IMAGE_WRITE_STATIC')
        options = @('/utf-8', '/w', '/FIcmake/MatterAutoremesherConfig.h'); libraries = @('matter_autoremesher.lib')
    }
    bc7enc = [ordered]@{ source = 'third_party/bc7enc'; languages = @('CXX'); defines = @(); options = @('/utf-8', '/w'); libraries = @('matter_bc7enc.lib') }
    box3d = [ordered]@{ source = 'third_party/box3d'; languages = @('C'); defines = @('B3_ENABLE_ASSERT'); options = @('/utf-8', '/w'); libraries = @('matter_box3d.lib') }
    flecs = [ordered]@{ source = 'third_party/flecs'; languages = @('C'); defines = @(); options = @('/utf-8', '/w'); libraries = @('matter_flecs.lib') }
    glfw = [ordered]@{ source = 'third_party/raylib/src/external/glfw'; languages = @('C'); defines = @('_GLFW_WIN32'); options = @('/utf-8', '/w'); libraries = @('matter_glfw.lib') }
    dear_imgui = [ordered]@{ source = 'third_party/imgui'; languages = @('CXX'); defines = @(); options = @('/utf-8', '/w'); libraries = @('matter_imgui.lib') }
    imguizmo = [ordered]@{ source = 'third_party/ImGuizmo'; languages = @('CXX'); defines = @(); options = @('/utf-8', '/w'); libraries = @('matter_imguizmo.lib') }
    ozz_animation = [ordered]@{ source = 'third_party/ozz-animation'; languages = @('CXX'); defines = @('_CRT_SECURE_NO_WARNINGS'); options = @('/utf-8', '/w'); libraries = @('matter_ozz_base.lib', 'matter_ozz_animation.lib', 'matter_ozz_offline.lib') }
    quickjs_ng = [ordered]@{ source = 'third_party/quickjs-ng'; languages = @('C'); defines = @('CONFIG_VERSION=0.10.0', 'WIN32_LEAN_AND_MEAN'); options = @('/utf-8', '/w'); libraries = @('matter_quickjs.lib') }
    vulkan_headers = [ordered]@{ source = 'third_party/Vulkan-Headers'; languages = @('C', 'CXX'); defines = @(); options = @(); libraries = @() }
}

$expectedNotices = @(
    'autoremesher_core', 'autoremesher_geogram', 'autoremesher_eigen',
    'autoremesher_isotropicremesher', 'autoremesher_zlib', 'autoremesher_rply',
    'autoremesher_libmeshb', 'autoremesher_stb_image', 'autoremesher_stb_image_write',
    'bc7enc', 'box3d', 'flecs', 'glfw', 'dear_imgui', 'imguizmo',
    'ozz_animation', 'quickjs_ng', 'vulkan_headers'
)
$nvidiaNotices = @('nvidia_physx', 'nvidia_cuda')

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Get-FileHashLower([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}

function New-DependencyManifest {
    $result = [ordered]@{}
    foreach ($entry in $expectedDependencies.GetEnumerator()) {
        $artifacts = @($entry.Value.libraries | ForEach-Object {
            [ordered]@{ name = $_; size = 128; sha256 = ('b' * 64) }
        })
        $result[$entry.Key] = [ordered]@{
            source = [ordered]@{
                path = $entry.Value.source
                git_tree = ('a' * 40)
                tree_sha256 = ('c' * 64)
            }
            build = [ordered]@{
                languages = @($entry.Value.languages)
                runtime = if ($entry.Value.libraries.Count -gt 0) { 'static' } else { 'none' }
                options = @($entry.Value.options)
                defines = @($entry.Value.defines)
            }
            artifacts = $artifacts
        }
    }
    return $result
}

function Write-Manifest(
    [string]$DistPath,
    [string]$CompilerId = 'MSVC',
    [string]$Configuration = 'RelWithDebInfo',
    [string[]]$RuntimeDlls = @('fixture_runtime.dll'),
    [bool]$TrackedDirty = $false,
    [bool]$Physx = $false,
    [bool]$Cuda = $false,
    [string[]]$Notices = $expectedNotices
) {
    $files = [ordered]@{}
    Get-ChildItem -LiteralPath $DistPath -Recurse -File |
        Where-Object { $_.Name -ne 'build_features.json' } |
        Sort-Object FullName |
        ForEach-Object {
            $relative = $_.FullName.Substring($DistPath.TrimEnd('\').Length + 1).Replace('\', '/')
            $files[$relative] = Get-FileHashLower $_.FullName
        }
    [ordered]@{
        schema_version = 2
        project = 'world_demo'
        configuration = $Configuration
        source = [ordered]@{
            revision = ('d' * 40)
            tracked_dirty = $TrackedDirty
            diff_sha256 = if ($TrackedDirty) { ('e' * 64) } else { $cleanDiffSha }
        }
        crt = [ordered]@{ linkage = 'static'; cmake = 'MultiThreaded$<$<CONFIG:Debug>:Debug>' }
        toolchain = [ordered]@{
            compiler = [ordered]@{ id = $CompilerId; version = $pinnedCompilerVersion }
            msvc_tools = $pinnedToolsVersion
            windows_sdk = $pinnedWindowsSdk
            vulkan_sdk = $pinnedVulkanSdk
        }
        dependencies = New-DependencyManifest
        notices = @($Notices)
        features = [ordered]@{
            autoremesher = $true
            streamline = $false
            physx = $Physx
            cuda = $Cuda
            vulkan_renderer = $true
        }
        runtime_dlls = @($RuntimeDlls | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) })
        files = $files
    } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $DistPath 'build_features.json') -Encoding utf8
}

function Initialize-RealPeFixture {
    $toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $RepositoryRoot
    $developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f `
        $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
    New-Item -ItemType Directory -Force -Path $fixtureBin, $developerDllDirectory | Out-Null
    $dllSource = Join-Path $scratch 'fixture_runtime.c'
    $exeSource = Join-Path $scratch 'fixture_launcher.c'
    $standaloneSource = Join-Path $scratch 'fixture_standalone.c'
    Set-Content -LiteralPath $dllSource -Encoding ascii -Value '__declspec(dllexport) int fixture_value(void) { return 42; }'
    Set-Content -LiteralPath $exeSource -Encoding ascii -Value @'
#include <stdio.h>
__declspec(dllimport) int fixture_value(void);
int main(void) {
    if (fixture_value() != 42) return 42;
    puts("MATTER_REGISTRATION_CENSUS_JSON={}");
    return 0;
}
'@
    Set-Content -LiteralPath $standaloneSource -Encoding ascii -Value @'
#include <stdio.h>
int main(void) {
    puts("MATTER_REGISTRATION_CENSUS_JSON={}");
    return 0;
}
'@
    $dll = Join-Path $developerDllDirectory 'fixture_runtime.dll'
    $importLibrary = Join-Path $developerDllDirectory 'fixture_runtime.lib'
    $exe = Join-Path $fixtureBin 'editor.exe'
    $standaloneExe = Join-Path $fixtureBin 'standalone-editor.exe'
    Push-Location $scratch
    try {
        $compileDll = '{0} && cl /nologo /LD /MT "{1}" /Fo"{2}" /link /OUT:"{3}" /IMPLIB:"{4}"' -f `
            $developerEnvironment, $dllSource, (Join-Path $scratch 'fixture_runtime.obj'), $dll, $importLibrary
        & $env:ComSpec /d /s /c $compileDll | Out-Null
        Assert-True ($LASTEXITCODE -eq 0) 'failed to compile the real fixture DLL'
        $compileExe = '{0} && cl /nologo /MT "{1}" "{2}" /Fo"{3}" /Fe:"{4}"' -f `
            $developerEnvironment, $exeSource, $importLibrary, (Join-Path $scratch 'fixture_launcher.obj'), $exe
        & $env:ComSpec /d /s /c $compileExe | Out-Null
        Assert-True ($LASTEXITCODE -eq 0) 'failed to compile the real fixture executable'
        $compileStandalone = '{0} && cl /nologo /MT "{1}" /Fo"{2}" /Fe:"{3}"' -f `
            $developerEnvironment, $standaloneSource, (Join-Path $scratch 'fixture_standalone.obj'), $standaloneExe
        & $env:ComSpec /d /s /c $compileStandalone | Out-Null
        Assert-True ($LASTEXITCODE -eq 0) 'failed to compile the standalone fixture executable'
    } finally {
        Pop-Location
    }
    return [pscustomobject]@{
        Editor = $exe
        StandaloneEditor = $standaloneExe
        RuntimeDll = $dll
    }
}

function New-ValidFixture(
    [string]$Name,
    [string]$Configuration = 'RelWithDebInfo',
    [switch]$WithoutRuntimeDll,
    [switch]$CompleteContent,
    [switch]$Physx
) {
    $dist = Join-Path $scratch $Name
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    $fixtureEditor = if ($Physx) { $script:peFixture.StandaloneEditor } else { $script:peFixture.Editor }
    Copy-Item -LiteralPath $fixtureEditor -Destination (Join-Path $dist 'editor.exe')
    if (-not $WithoutRuntimeDll) {
        $runtimeName = if ($Physx) { 'PhysXGpu_64.dll' } else { 'fixture_runtime.dll' }
        Copy-Item -LiteralPath $script:peFixture.RuntimeDll -Destination (Join-Path $dist $runtimeName)
    }
    if ($Configuration -eq 'RelWithDebInfo') {
        Set-Content -LiteralPath (Join-Path $dist 'editor.pdb') -Value 'fixture PDB identity' -Encoding ascii
    }
    if ($CompleteContent) {
        Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'projects\world_demo') `
            -Destination (Join-Path $dist 'projects\world_demo') -Recurse
        Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'MatterEngine3\shared-lib') `
            -Destination (Join-Path $dist 'MatterEngine3\shared-lib') -Recurse
    } else {
        New-Item -ItemType Directory -Force `
            -Path (Join-Path $dist 'projects\world_demo'), (Join-Path $dist 'MatterEngine3\shared-lib') | Out-Null
        Set-Content -LiteralPath (Join-Path $dist 'projects\world_demo\fixture-scene.js') `
            -Value '// package fixture project content' -Encoding ascii
        Set-Content -LiteralPath (Join-Path $dist 'MatterEngine3\shared-lib\fixture-runtime.js') `
            -Value '// package fixture engine shared library' -Encoding ascii
    }
    # The real stager excludes developer caches. Complete-content fixtures copy
    # the live project tree, so remove that copied cache before constructing the
    # explicit accepted-network fixture below.
    $copiedProjectCache = Join-Path $dist 'projects\world_demo\.cache'
    if (Test-Path -LiteralPath $copiedProjectCache) {
        Remove-Item -LiteralPath $copiedProjectCache -Recurse -Force
    }
    $notice = @('MatterEngine third-party notices')
    foreach ($label in $expectedNotices) {
        $notice += "===== ${label}: fixture-license.txt ====="
        $notice += "Fixture license content for ${label}. $([string]('x' * 80))"
    }
    if ($Physx) {
        $licenses = Join-Path $dist 'licenses'
        New-Item -ItemType Directory -Force -Path $licenses | Out-Null
        Set-Content -LiteralPath (Join-Path $licenses 'NVIDIA_PhysX_LICENSE.md') `
            -Value 'Fixture NVIDIA PhysX license content.' -Encoding utf8
        Set-Content -LiteralPath (Join-Path $licenses 'NVIDIA_CUDA_EULA.txt') `
            -Value 'Fixture NVIDIA CUDA EULA content.' -Encoding utf8
        foreach ($label in $nvidiaNotices) {
            $notice += "===== ${label}: licenses/fixture.txt ====="
            $notice += "Fixture license content for ${label}. $([string]('x' * 80))"
        }
        $hydrologyCache = Join-Path $dist 'projects\world_demo\.cache\RiverHydrology\hydrology'
        New-Item -ItemType Directory -Force `
            -Path $hydrologyCache, (Join-Path $hydrologyCache 'sections'), `
                  (Join-Path $hydrologyCache 'handoffs') | Out-Null
        Set-Content -LiteralPath (Join-Path $hydrologyCache 'network.mhyn') `
            -Value 'Accepted MHYDNET fixture content.' -Encoding ascii
        Set-Content -LiteralPath (Join-Path $hydrologyCache 'sections\upper.mhyd') `
            -Value 'Accepted upper MHYD fixture content.' -Encoding ascii
        Set-Content -LiteralPath (Join-Path $hydrologyCache 'sections\lower.mhyd') `
            -Value 'Accepted lower MHYD fixture content.' -Encoding ascii
        Set-Content -LiteralPath (Join-Path $hydrologyCache 'handoffs\pool-one.mhyd') `
            -Value 'Accepted handoff MHYD fixture content.' -Encoding ascii
    }
    Set-Content -LiteralPath (Join-Path $dist 'THIRD_PARTY_NOTICES.txt') -Value $notice -Encoding utf8
    $runtime = if ($WithoutRuntimeDll) { @() } elseif ($Physx) { @('PhysXGpu_64.dll') } else { @('fixture_runtime.dll') }
    $notices = if ($Physx) { @($expectedNotices + $nvidiaNotices) } else { @($expectedNotices) }
    Write-Manifest $dist -Configuration $Configuration -RuntimeDlls $runtime `
        -Physx $Physx.IsPresent -Cuda $Physx.IsPresent -Notices $notices
    return $dist
}

function Invoke-Checker(
    [string]$DistPath,
    [string]$Dumpbin,
    [string[]]$FakeImports
) {
    $savedImports = $env:MATTER_PACKAGE_TEST_IMPORTS
    try {
        if ($null -ne $FakeImports) { $env:MATTER_PACKAGE_TEST_IMPORTS = $FakeImports -join ';' }
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $checker, '-DistPath', $DistPath)
        if ($Dumpbin) { $arguments += @('-DumpbinPath', $Dumpbin) }
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $output = @(& powershell.exe @arguments 2>&1)
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previous
        }
        return [pscustomobject]@{ ExitCode = $exitCode; Output = $output -join "`n" }
    } finally {
        $env:MATTER_PACKAGE_TEST_IMPORTS = $savedImports
    }
}

function Assert-Rejected(
    [string]$Name,
    [string]$DistPath,
    [string]$Pattern,
    [string]$Dumpbin,
    [string[]]$FakeImports
) {
    $result = Invoke-Checker $DistPath $Dumpbin $FakeImports
    Assert-True ($result.ExitCode -ne 0) "$Name unexpectedly passed"
    Assert-True ($result.Output -match $Pattern) "$Name did not report '$Pattern':`n$($result.Output)"
    Write-Output "package fixture: $Name rejected"
}

try {
    New-Item -ItemType Directory -Force -Path $scratch | Out-Null
    Set-Content -LiteralPath $fakeDumpbin -Encoding utf8 -Value @'
$imports = $env:MATTER_PACKAGE_TEST_IMPORTS -split ';' | Where-Object { $_ }
Write-Output 'Microsoft (R) COFF/PE Dumper Version fixture'
Write-Output '  Image has the following dependencies:'
$imports | ForEach-Object { Write-Output "    $_" }
exit 0
'@
    $script:peFixture = Initialize-RealPeFixture

    $missingExe = New-ValidFixture 'missing-exe'
    Remove-Item -LiteralPath (Join-Path $missingExe 'editor.exe')
    Write-Manifest $missingExe
    Assert-Rejected 'missing executable' $missingExe 'editor\.exe.*missing|missing.*editor\.exe' $fakeDumpbin @('KERNEL32.dll')

    $forbidden = New-ValidFixture 'forbidden-gnu-import'
    Assert-Rejected 'forbidden GNU import' $forbidden 'libstdc\+\+|forbidden' $fakeDumpbin @('KERNEL32.dll', 'libstdc++-6.dll')

    $dynamicMsvcCrt = New-ValidFixture 'dynamic-msvc-crt-import'
    Assert-Rejected 'dynamic MSVC CRT import' $dynamicMsvcCrt `
        'VCRUNTIME140|MSVCP140|dynamic.*CRT|static.*CRT' $fakeDumpbin `
        @('KERNEL32.dll', 'fixture_runtime.dll', 'VCRUNTIME140_1.dll', 'MSVCP140.dll')

    $missingNotice = New-ValidFixture 'missing-notice'
    Remove-Item -LiteralPath (Join-Path $missingNotice 'THIRD_PARTY_NOTICES.txt')
    Write-Manifest $missingNotice
    Assert-Rejected 'missing notices' $missingNotice 'notice.*missing|missing.*notice' $fakeDumpbin @('KERNEL32.dll')

    $missingManifest = New-ValidFixture 'missing-manifest'
    Remove-Item -LiteralPath (Join-Path $missingManifest 'build_features.json')
    Assert-Rejected 'missing manifest' $missingManifest 'build_features\.json.*missing|missing.*build_features\.json' $fakeDumpbin @('KERNEL32.dll')

    $compilerMismatch = New-ValidFixture 'compiler-mismatch'
    Write-Manifest $compilerMismatch 'GNU'
    Assert-Rejected 'compiler mismatch' $compilerMismatch 'compiler.*GNU|MSVC.*required' $fakeDumpbin @('KERNEL32.dll')

    $extraFile = New-ValidFixture 'extra-file'
    Set-Content -LiteralPath (Join-Path $extraFile 'stale.tmp') -Value 'stale' -Encoding ascii
    Assert-Rejected 'extra file' $extraFile 'not listed|extra|stale' $fakeDumpbin @('KERNEL32.dll')

    $missingPdb = New-ValidFixture 'missing-pdb'
    Remove-Item -LiteralPath (Join-Path $missingPdb 'editor.pdb')
    Write-Manifest $missingPdb
    Assert-Rejected 'missing PDB' $missingPdb 'PDB.*missing|missing.*PDB' $fakeDumpbin @('KERNEL32.dll')

    $extraPdb = New-ValidFixture 'extra-pdb' 'Release'
    Set-Content -LiteralPath (Join-Path $extraPdb 'editor.pdb') -Value 'unexpected' -Encoding ascii
    Write-Manifest $extraPdb -Configuration 'Release'
    Assert-Rejected 'extra PDB' $extraPdb 'PDB.*unexpected|unexpected.*PDB' $fakeDumpbin @('KERNEL32.dll')

    $runtimeMismatch = New-ValidFixture 'runtime-mismatch'
    Write-Manifest $runtimeMismatch -RuntimeDlls @()
    Assert-Rejected 'runtime_dlls mismatch' $runtimeMismatch 'runtime_dlls.*mismatch|staged DLL' $fakeDumpbin @('KERNEL32.dll')

    $hashTamper = New-ValidFixture 'hash-tamper'
    Add-Content -LiteralPath (Join-Path $hashTamper 'THIRD_PARTY_NOTICES.txt') -Value 'tampered'
    Assert-Rejected 'hash tamper' $hashTamper 'hash mismatch' $fakeDumpbin @('KERNEL32.dll')

    $unusedDll = New-ValidFixture 'unused-dll'
    Copy-Item -LiteralPath $script:peFixture.RuntimeDll -Destination (Join-Path $unusedDll 'unused.dll')
    Write-Manifest $unusedDll -RuntimeDlls @('fixture_runtime.dll', 'unused.dll')
    Assert-Rejected 'unused DLL' $unusedDll 'unused.*DLL|not reachable' '' $null

    $missingImported = New-ValidFixture 'missing-imported' -WithoutRuntimeDll
    Assert-Rejected 'missing imported DLL' $missingImported 'fixture_runtime\.dll.*missing|missing.*fixture_runtime\.dll' '' $null

    $noticeContent = New-ValidFixture 'missing-notice-component'
    (Get-Content -LiteralPath (Join-Path $noticeContent 'THIRD_PARTY_NOTICES.txt')) |
        Where-Object { $_ -notmatch 'autoremesher_stb_image_write' } |
        Set-Content -LiteralPath (Join-Path $noticeContent 'THIRD_PARTY_NOTICES.txt') -Encoding utf8
    Write-Manifest $noticeContent
    Assert-Rejected 'missing notice component' $noticeContent 'autoremesher_stb_image_write|notice component' $fakeDumpbin @('KERNEL32.dll')

    $emptyNotice = New-ValidFixture 'empty-notice-content'
    (Get-Content -LiteralPath (Join-Path $emptyNotice 'THIRD_PARTY_NOTICES.txt')) |
        Where-Object { $_ -notmatch '^Fixture license content for autoremesher_stb_image_write\.' } |
        Set-Content -LiteralPath (Join-Path $emptyNotice 'THIRD_PARTY_NOTICES.txt') -Encoding utf8
    Write-Manifest $emptyNotice
    Assert-Rejected 'empty notice content' $emptyNotice 'stb_image_write.*substantive|license content' $fakeDumpbin @('KERNEL32.dll')

    $dirtyRelease = New-ValidFixture 'dirty-release'
    Write-Manifest $dirtyRelease -TrackedDirty $true
    Assert-Rejected 'dirty release provenance' $dirtyRelease 'clean tracked source tree|tracked.*clean' $fakeDumpbin @('KERNEL32.dll')

    $dependencyOptions = New-ValidFixture 'dependency-options'
    $dependencyManifestPath = Join-Path $dependencyOptions 'build_features.json'
    $dependencyJson = Get-Content -LiteralPath $dependencyManifestPath -Raw | ConvertFrom-Json
    $dependencyJson.dependencies.glfw.build.options = @('/w')
    $dependencyJson | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $dependencyManifestPath -Encoding utf8
    Assert-Rejected 'dependency options mismatch' $dependencyOptions 'glfw options.*mismatch|dependency.*options' $fakeDumpbin @('KERNEL32.dll')

    $pathOnly = New-ValidFixture 'developer-path-only' -WithoutRuntimeDll
    $savedPath = $env:PATH
    try {
        $env:PATH = "$developerDllDirectory;$savedPath"
        $proof = @(& (Join-Path $pathOnly 'editor.exe') 2>&1)
        Assert-True ($LASTEXITCODE -eq 0) "real developer-PATH fixture did not launch:`n$($proof -join "`n")"
    } finally {
        $env:PATH = $savedPath
    }
    Assert-Rejected 'developer-PATH-only launch' $pathOnly 'fixture_runtime\.dll.*missing|clean.PATH|developer.PATH' '' $null

    $featureMismatch = New-ValidFixture 'physx-cuda-feature-mismatch'
    Write-Manifest $featureMismatch -Physx $true -Cuda $false
    Assert-Rejected 'PhysX/CUDA feature mismatch' $featureMismatch `
        'PhysX.*CUDA|CUDA.*PhysX|feature.*mismatch' $fakeDumpbin @('KERNEL32.dll', 'fixture_runtime.dll')

    $runtimeAlias = New-ValidFixture 'physx-runtime-alias' -Physx
    Move-Item -LiteralPath (Join-Path $runtimeAlias 'PhysXGpu_64.dll') `
        -Destination (Join-Path $runtimeAlias 'PhysXGpu_64-copy.dll')
    Write-Manifest $runtimeAlias -RuntimeDlls @('PhysXGpu_64-copy.dll') `
        -Physx $true -Cuda $true -Notices @($expectedNotices + $nvidiaNotices)
    Assert-Rejected 'PhysX runtime alias' $runtimeAlias `
        'PhysXGpu_64\.dll|runtime alias|exact.*runtime' $fakeDumpbin @('KERNEL32.dll')

    $missingNvidiaNotice = New-ValidFixture 'physx-missing-nvidia-notice' -Physx
    Remove-Item -LiteralPath (Join-Path $missingNvidiaNotice 'licenses\NVIDIA_CUDA_EULA.txt')
    Write-Manifest $missingNvidiaNotice -RuntimeDlls @('PhysXGpu_64.dll') `
        -Physx $true -Cuda $true -Notices @($expectedNotices + $nvidiaNotices)
    Assert-Rejected 'missing NVIDIA notice file' $missingNvidiaNotice `
        'NVIDIA_CUDA_EULA|NVIDIA.*notice|CUDA.*EULA' $fakeDumpbin @('KERNEL32.dll')

    $missingHydrologyArtifact = New-ValidFixture 'physx-missing-hydrology-artifact' -Physx
    Remove-Item -LiteralPath `
        (Join-Path $missingHydrologyArtifact 'projects\world_demo\.cache\RiverHydrology\hydrology\sections\lower.mhyd')
    Write-Manifest $missingHydrologyArtifact -RuntimeDlls @('PhysXGpu_64.dll') `
        -Physx $true -Cuda $true -Notices @($expectedNotices + $nvidiaNotices)
    Assert-Rejected 'missing accepted hydrology artifact' $missingHydrologyArtifact `
        'RiverHydrology|\.mhyd|hydrology (artifact|network)' $fakeDumpbin @('KERNEL32.dll')

    $valid = New-ValidFixture 'valid' -CompleteContent
    $validResult = Invoke-Checker $valid '' $null
    Assert-True ($validResult.ExitCode -eq 0) "valid package failed:`n$($validResult.Output)"
    Assert-True ($validResult.Output -match 'MSVC package: PASS') 'valid package omitted PASS summary'
    $validPhysx = New-ValidFixture 'valid-physx' -CompleteContent -Physx
    $validPhysxResult = Invoke-Checker $validPhysx '' $null
    Assert-True ($validPhysxResult.ExitCode -eq 0) "valid PhysX package failed:`n$($validPhysxResult.Output)"
    Assert-True ($validPhysxResult.Output -match 'MSVC package: PASS') `
        'valid PhysX package omitted PASS summary'
    Write-Output 'Windows MSVC package fixtures: PASS (24/24; PhysX runtime/notices/network, real recursive PE closure, and standalone dumpbin discovery)'
} finally {
    if ($env:MATTER_KEEP_PACKAGE_TESTS) {
        Write-Output "MATTER_PACKAGE_TEST_SCRATCH=$scratch"
    } elseif (Test-Path -LiteralPath $scratch) {
        Remove-Item -LiteralPath $scratch -Recurse -Force
    }
}
