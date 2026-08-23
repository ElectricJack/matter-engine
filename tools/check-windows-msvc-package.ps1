[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DistPath,
    [string]$DumpbinPath,
    [int]$LaunchTimeoutMilliseconds = 30000
)

$ErrorActionPreference = 'Stop'
trap {
    $line = $_.InvocationInfo.ScriptLineNumber
    $stack = $_.ScriptStackTrace
    [Console]::Error.WriteLine("package checker failed at line ${line}: $($_.Exception.Message)`n$stack")
    exit 1
}
$pinnedCompilerVersion = '19.44.35211'
$pinnedToolsVersion = '14.44.35207'
$pinnedWindowsSdk = '10.0.26100.0'
$pinnedVulkanSdk = '1.4.357.0'
$cleanDiffSha = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
$hex40 = '^[0-9a-f]{40}$'
$hex64 = '^[0-9a-f]{64}$'

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

$dist = if (Test-Path -LiteralPath $DistPath -PathType Container) {
    (Resolve-Path -LiteralPath $DistPath).Path
} else {
    throw "MSVC package directory is missing: $DistPath"
}

function Require-PackageFile([string]$RelativePath, [string]$Description) {
    $path = Join-Path $dist $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "$Description is missing: $RelativePath"
    }
    return (Resolve-Path -LiteralPath $path).Path
}

function Get-Sha256Lower([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}

function Get-RelativePackagePath([string]$Path) {
    return $Path.Substring($dist.TrimEnd('\').Length + 1).Replace('\', '/')
}

function Assert-ExactStrings([string]$Label, [object[]]$Actual, [object[]]$Expected) {
    $actualStrings = @($Actual | ForEach-Object { [string]$_ })
    $expectedStrings = @($Expected | ForEach-Object { [string]$_ })
    if (($actualStrings -join "`n") -cne ($expectedStrings -join "`n")) {
        throw "$Label mismatch: expected [$($expectedStrings -join ', ')], got [$($actualStrings -join ', ')]"
    }
}

function Assert-ExactProperties([string]$Label, [object]$Object, [string[]]$Expected) {
    if ($null -eq $Object) { throw "$Label is missing" }
    $actual = @($Object.psobject.Properties.Name | Sort-Object)
    $wanted = @($Expected | Sort-Object)
    Assert-ExactStrings "$Label properties" $actual $wanted
}

function Resolve-DumpbinExecutable {
    if ($DumpbinPath) {
        if (-not (Test-Path -LiteralPath $DumpbinPath -PathType Leaf)) {
            throw "dumpbin executable is missing: $DumpbinPath"
        }
        return (Resolve-Path -LiteralPath $DumpbinPath).Path
    }

    $fromPath = Get-Command dumpbin.exe -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Source -First 1
    if ($fromPath -and (Test-Path -LiteralPath $fromPath -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $fromPath).Path
    }

    $candidates = @()
    if ($env:VCToolsInstallDir) {
        $candidates += Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64\dumpbin.exe'
    }
    if ($env:VSINSTALLDIR) {
        $candidates += Join-Path $env:VSINSTALLDIR "VC\Tools\MSVC\$pinnedToolsVersion\bin\Hostx64\x64\dumpbin.exe"
    }

    $vswhere = if (${env:ProgramFiles(x86)}) {
        Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    } else { '' }
    if ($vswhere -and (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        $installations = @(& $vswhere -version '[17.0,18.0)' -products '*' `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null)
        foreach ($installation in ($installations | Where-Object { $_ })) {
            $candidates += Join-Path ([string]$installation) "VC\Tools\MSVC\$pinnedToolsVersion\bin\Hostx64\x64\dumpbin.exe"
        }
    }
    foreach ($edition in @('Community', 'Professional', 'Enterprise', 'BuildTools')) {
        $candidates += "C:\Program Files\Microsoft Visual Studio\2022\$edition\VC\Tools\MSVC\$pinnedToolsVersion\bin\Hostx64\x64\dumpbin.exe"
    }
    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "dumpbin.exe from pinned MSVC tools $pinnedToolsVersion was not found via -DumpbinPath, PATH, VS environment, vswhere, or pinned VS paths"
}

$resolvedDumpbin = Resolve-DumpbinExecutable

function Invoke-DumpbinDependents([string]$Binary) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $resolvedDumpbin /dependents $Binary 2>&1)
        $exitCode = $LASTEXITCODE
        if ($null -eq $exitCode) { $exitCode = 0 }
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($exitCode -ne 0) {
        throw "dumpbin /dependents failed for $Binary (exit $exitCode):`n$($output -join [Environment]::NewLine)"
    }
    return @($output | ForEach-Object { [string]$_ } | ForEach-Object {
        if ($_ -match '^\s*([A-Za-z0-9_.+\-]+\.dll)\s*$') { $matches[1] }
    } | Where-Object { $_ } | Select-Object -Unique)
}

function Assert-CleanPathLaunch([string]$Editor) {
    $system32 = Join-Path $env:SystemRoot 'System32'
    $temp = [IO.Path]::GetTempPath().TrimEnd('\')
    $powershell = Join-Path $system32 'WindowsPowerShell\v1.0\powershell.exe'
    $quotedEditor = $Editor.Replace("'", "''")
    $quotedDist = $dist.Replace("'", "''")
    $quotedSystemRoot = ([string]$env:SystemRoot).Replace("'", "''")
    $quotedWindir = ([string]$env:WINDIR).Replace("'", "''")
    $quotedSystem32 = $system32.Replace("'", "''")
    $quotedTemp = $temp.Replace("'", "''")
    $launchScript = @"
Get-ChildItem Env: | ForEach-Object { Remove-Item -LiteralPath ('Env:' + `$_.Name) }
`$env:SystemRoot = '$quotedSystemRoot'
`$env:WINDIR = '$quotedWindir'
`$env:PATH = '$quotedSystem32'
`$env:TMP = '$quotedTemp'
`$env:TEMP = '$quotedTemp'
`$env:MATTER_REGISTRATION_CENSUS = '1'
Set-Location -LiteralPath '$quotedDist'
& '$quotedEditor'
exit `$LASTEXITCODE
"@
    $encodedCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($launchScript))
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $powershell
    $startInfo.Arguments = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encodedCommand"
    $startInfo.WorkingDirectory = $dist
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        try {
            if (-not $process.Start()) { throw 'process did not start' }
        } catch {
            throw "clean-PATH package launch failed: $($_.Exception.Message)"
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($LaunchTimeoutMilliseconds)) {
            $process.Kill()
            $process.WaitForExit()
            throw "clean-PATH package launch exceeded $LaunchTimeoutMilliseconds ms"
        }
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        if ($process.ExitCode -ne 0) {
            throw "clean-PATH package launch exited $($process.ExitCode):`n$stdout`n$stderr"
        }
        if (($stdout + $stderr) -notmatch 'MATTER_REGISTRATION_CENSUS_JSON=\{') {
            throw "clean-PATH package launch did not reach the registration census:`n$stdout`n$stderr"
        }
    } finally {
        $process.Dispose()
    }
}

$editor = Require-PackageFile 'editor.exe' 'editor.exe executable'
$noticePath = Require-PackageFile 'THIRD_PARTY_NOTICES.txt' 'Third-party notice bundle'
$manifestPath = Require-PackageFile 'build_features.json' 'build_features.json manifest'

try {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
} catch {
    throw "build_features.json is not valid JSON: $($_.Exception.Message)"
}
Assert-ExactProperties 'manifest' $manifest @(
    'schema_version', 'project', 'configuration', 'source', 'crt', 'toolchain',
    'dependencies', 'notices', 'features', 'runtime_dlls', 'files'
)
if ($manifest.schema_version -ne 2) { throw 'build_features.json schema_version must be 2' }
if ([string]$manifest.project -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
    throw "manifest project is not a safe basename: '$($manifest.project)'"
}
if ($manifest.configuration -notin @('Debug', 'RelWithDebInfo', 'Release')) {
    throw "unsupported package configuration '$($manifest.configuration)'"
}

Assert-ExactProperties 'source' $manifest.source @('revision', 'tracked_dirty', 'diff_sha256')
if ([string]$manifest.source.revision -cnotmatch $hex40) { throw 'source revision must be a lowercase 40-hex Git identity' }
if ([string]$manifest.source.diff_sha256 -cnotmatch $hex64) { throw 'source diff_sha256 must be lowercase SHA-256' }
if ($manifest.source.tracked_dirty -isnot [bool]) { throw 'source tracked_dirty must be boolean' }
if (-not $manifest.source.tracked_dirty -and $manifest.source.diff_sha256 -ne $cleanDiffSha) {
    throw 'clean source must carry the SHA-256 identity of an empty tracked diff'
}
if ($manifest.configuration -in @('RelWithDebInfo', 'Release') -and $manifest.source.tracked_dirty) {
    throw 'release package requires a clean tracked source tree'
}

Assert-ExactProperties 'crt' $manifest.crt @('linkage', 'cmake')
if ($manifest.crt.linkage -ne 'static' -or $manifest.crt.cmake -ne 'MultiThreaded$<$<CONFIG:Debug>:Debug>') {
    throw 'package must record the exact static MSVC CRT policy'
}
Assert-ExactProperties 'toolchain' $manifest.toolchain @('compiler', 'msvc_tools', 'windows_sdk', 'vulkan_sdk')
Assert-ExactProperties 'toolchain.compiler' $manifest.toolchain.compiler @('id', 'version')
if ($manifest.toolchain.compiler.id -ne 'MSVC') { throw "MSVC compiler is required; manifest reports '$($manifest.toolchain.compiler.id)'" }
if ($manifest.toolchain.compiler.version -ne $pinnedCompilerVersion) { throw "compiler version must be $pinnedCompilerVersion" }
if ($manifest.toolchain.msvc_tools -ne $pinnedToolsVersion) { throw "MSVC tools must be $pinnedToolsVersion" }
if ($manifest.toolchain.windows_sdk -ne $pinnedWindowsSdk) { throw "Windows SDK must be $pinnedWindowsSdk" }
if ($manifest.toolchain.vulkan_sdk -ne $pinnedVulkanSdk) { throw "Vulkan SDK must be $pinnedVulkanSdk" }

Assert-ExactProperties 'dependencies' $manifest.dependencies @($expectedDependencies.Keys)
foreach ($entry in $expectedDependencies.GetEnumerator()) {
    $identity = $entry.Key
    $expected = $entry.Value
    $actual = $manifest.dependencies.$identity
    Assert-ExactProperties "dependency $identity" $actual @('source', 'build', 'artifacts')
    Assert-ExactProperties "dependency $identity source" $actual.source @('path', 'git_tree', 'tree_sha256')
    if ($actual.source.path -cne $expected.source) { throw "dependency $identity source path mismatch" }
    if ([string]$actual.source.git_tree -cnotmatch $hex40) { throw "dependency $identity git_tree must be lowercase 40-hex" }
    if ([string]$actual.source.tree_sha256 -cnotmatch $hex64) { throw "dependency $identity tree_sha256 must be lowercase SHA-256" }
    Assert-ExactProperties "dependency $identity build" $actual.build @('languages', 'runtime', 'options', 'defines')
    Assert-ExactStrings "dependency $identity languages" @($actual.build.languages) @($expected.languages)
    Assert-ExactStrings "dependency $identity defines" @($actual.build.defines) @($expected.defines)
    Assert-ExactStrings "dependency $identity options" @($actual.build.options) @($expected.options)
    $expectedRuntime = if ($expected.libraries.Count -gt 0) { 'static' } else { 'none' }
    if ($actual.build.runtime -ne $expectedRuntime) { throw "dependency $identity runtime mismatch" }
    $artifacts = @($actual.artifacts)
    Assert-ExactStrings "dependency $identity artifacts" @($artifacts | ForEach-Object { $_.name }) @($expected.libraries)
    foreach ($artifact in $artifacts) {
        Assert-ExactProperties "dependency $identity artifact" $artifact @('name', 'size', 'sha256')
        if ([long]$artifact.size -le 0) { throw "dependency $identity artifact $($artifact.name) has invalid size" }
        if ([string]$artifact.sha256 -cnotmatch $hex64) { throw "dependency $identity artifact $($artifact.name) has invalid SHA-256" }
    }
}

Assert-ExactStrings 'notice component list' @($manifest.notices) $expectedNotices
$noticeText = Get-Content -LiteralPath $noticePath -Raw
foreach ($label in $expectedNotices) {
    $escaped = [regex]::Escape($label)
    $match = [regex]::Match(
        $noticeText,
        "(?ms)^===== ${escaped}: [^`r`n]+ =====\r?\n(?<content>.*?)(?=^===== |\z)"
    )
    if (-not $match.Success) { throw "required notice component '$label' is missing" }
    if ($match.Groups['content'].Value.Trim().Length -lt 32) {
        throw "required notice component '$label' has no substantive license content"
    }
}

Assert-ExactProperties 'features' $manifest.features @('autoremesher', 'streamline', 'physx', 'cuda', 'vulkan_renderer')
if (-not $manifest.features.vulkan_renderer) { throw 'Vulkan renderer feature must be enabled' }
foreach ($flag in @('autoremesher', 'streamline', 'physx', 'cuda', 'vulkan_renderer')) {
    if ($manifest.features.$flag -isnot [bool]) { throw "feature $flag must be boolean" }
}

$hasPdb = Test-Path -LiteralPath (Join-Path $dist 'editor.pdb') -PathType Leaf
if ($manifest.configuration -eq 'RelWithDebInfo' -and -not $hasPdb) { throw 'RelWithDebInfo PDB is missing: editor.pdb' }
if ($manifest.configuration -ne 'RelWithDebInfo' -and $hasPdb) { throw "PDB is unexpected for $($manifest.configuration) package" }

$manifestFiles = @($manifest.files.psobject.Properties)
if ($manifestFiles.Count -eq 0) { throw 'build_features.json files map is empty' }
$manifestFileSet = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $manifestFiles) {
    $relative = [string]$entry.Name
    if ($relative -eq 'build_features.json') { throw 'build_features.json must be the explicit self-hash exception, not listed in files' }
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|/)\.\.(/|$)' -or $relative -match '\\') {
        throw "manifest contains unsafe package path '$relative'"
    }
    if ([string]$entry.Value -cnotmatch $hex64) { throw "manifest hash for $relative is not lowercase SHA-256" }
    if (-not $manifestFileSet.Add($relative)) { throw "manifest contains duplicate file '$relative'" }
    $path = Join-Path $dist $relative.Replace('/', '\')
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "manifest file is missing: $relative" }
    $actualHash = Get-Sha256Lower $path
    if ($actualHash -cne [string]$entry.Value) {
        throw "manifest hash mismatch for ${relative}: expected $($entry.Value), actual $actualHash"
    }
}

$diskFileSet = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
Get-ChildItem -LiteralPath $dist -Recurse -File | ForEach-Object {
    $relative = Get-RelativePackagePath $_.FullName
    if ($relative -ne 'build_features.json') { [void]$diskFileSet.Add($relative) }
}
foreach ($relative in $diskFileSet) {
    if (-not $manifestFileSet.Contains($relative)) { throw "package contains extra file not listed in manifest: $relative" }
}
foreach ($relative in $manifestFileSet) {
    if (-not $diskFileSet.Contains($relative)) { throw "manifest file is absent from package: $relative" }
}

$runtimeDlls = @($manifest.runtime_dlls | ForEach-Object { [string]$_ })
if (($runtimeDlls | Select-Object -Unique).Count -ne $runtimeDlls.Count) { throw 'runtime_dlls contains duplicates' }
foreach ($runtime in $runtimeDlls) {
    if ($runtime -notmatch '^[A-Za-z0-9_.+\-]+\.dll$' -or [IO.Path]::GetFileName($runtime) -ne $runtime) {
        throw "runtime_dlls contains invalid staged DLL name '$runtime'"
    }
}
$stagedDlls = @(Get-ChildItem -LiteralPath $dist -Recurse -File -Filter '*.dll' | ForEach-Object { Get-RelativePackagePath $_.FullName })
Assert-ExactStrings 'runtime_dlls/staged DLL closure' @($stagedDlls | Sort-Object) @($runtimeDlls | Sort-Object)

$forbiddenImports = '^(libstdc\+\+|libgcc|libwinpthread|opengl32).*\.dll$'
$dynamicMsvcCrtImports = '^(vcruntime|msvcp|concrt|msvcr|ucrtbase|vccorlib)[a-z0-9_.-]*\.dll$'
$systemDirectory = Join-Path $env:SystemRoot 'System32'
$pending = New-Object 'System.Collections.Generic.Queue[string]'
$pending.Enqueue($editor)
$visited = @{}
$allImports = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$reachableStaged = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
while ($pending.Count -gt 0) {
    $binary = $pending.Dequeue()
    if ($visited.ContainsKey($binary)) { continue }
    $visited[$binary] = $true
    foreach ($import in (Invoke-DumpbinDependents $binary)) {
        [void]$allImports.Add($import)
        if ($import -match $forbiddenImports) {
            throw "forbidden GNU/OpenGL import '$import' in $([IO.Path]::GetFileName($binary))"
        }
        if ($import -match $dynamicMsvcCrtImports) {
            throw "dynamic MSVC CRT import '$import' in $([IO.Path]::GetFileName($binary)) contradicts the static CRT package policy"
        }
        $staged = Join-Path $dist $import
        if (Test-Path -LiteralPath $staged -PathType Leaf) {
            [void]$reachableStaged.Add($import)
            $pending.Enqueue((Resolve-Path -LiteralPath $staged).Path)
            continue
        }
        $system = Join-Path $systemDirectory $import
        if (-not (Test-Path -LiteralPath $system -PathType Leaf)) {
            throw "required runtime DLL '$import' is missing from the package and Windows System32; developer-PATH-only dependencies are forbidden"
        }
    }
}
foreach ($runtime in $runtimeDlls) {
    if (-not $reachableStaged.Contains($runtime)) { throw "staged runtime DLL '$runtime' is unused and not reachable from editor.exe" }
}
Assert-ExactStrings 'reachable staged DLL/runtime_dlls closure' @($reachableStaged | Sort-Object) @($runtimeDlls | Sort-Object)

Assert-CleanPathLaunch $editor
Write-Output ("MSVC package: PASS ({0} hashed files + manifest, {1} import(s), {2} runtime DLL(s), clean-PATH launch)" -f `
    $manifestFiles.Count, $allImports.Count, $runtimeDlls.Count)
