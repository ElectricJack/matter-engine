[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DistPath,
    [string]$DumpbinPath,
    [int]$LaunchTimeoutMilliseconds = 30000
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
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

function Invoke-DumpbinDependents([string]$Binary) {
    if ($DumpbinPath) {
        if (-not (Test-Path -LiteralPath $DumpbinPath -PathType Leaf)) {
            throw "dumpbin executable is missing: $DumpbinPath"
        }
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $output = @(& $DumpbinPath /dependents $Binary 2>&1)
            $exitCode = $LASTEXITCODE
            if ($null -eq $exitCode) { $exitCode = 0 }
        } finally {
            $ErrorActionPreference = $previous
        }
    } else {
        $modulePath = Join-Path $repositoryRoot 'tools\windows\MatterWindowsToolchain.psm1'
        Import-Module $modulePath -Force
        $toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot
        $developerEnvironment = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2}' -f `
            $toolchain.VsDevCmd, $toolchain.WindowsSdkVersion, $toolchain.MsvcToolsVersion
        $command = '{0} && dumpbin.exe /dependents "{1}"' -f $developerEnvironment, $Binary
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $output = @(& $env:ComSpec /d /s /c $command 2>&1)
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previous
        }
    }
    if ($exitCode -ne 0) {
        throw "dumpbin /dependents failed for $Binary (exit $exitCode):`n$($output -join [Environment]::NewLine)"
    }
    return @($output | ForEach-Object { [string]$_ } | ForEach-Object {
        if ($_ -match '^\s*([A-Za-z0-9_.+\-]+\.dll)\s*$') { $matches[1] }
    } | Where-Object { $_ } | Select-Object -Unique)
}

function Assert-CleanPathLaunch([string]$Editor) {
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Editor
    $startInfo.WorkingDirectory = $dist
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.EnvironmentVariables.Clear()
    $system32 = Join-Path $env:SystemRoot 'System32'
    $temp = [IO.Path]::GetTempPath().TrimEnd('\')
    $cleanEnvironment = [ordered]@{
        SystemRoot = $env:SystemRoot
        WINDIR = $env:WINDIR
        PATH = $system32
        TMP = $temp
        TEMP = $temp
        MATTER_REGISTRATION_CENSUS = '1'
    }
    foreach ($entry in $cleanEnvironment.GetEnumerator()) {
        $startInfo.EnvironmentVariables[$entry.Key] = [string]$entry.Value
    }

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
$notice = Require-PackageFile 'THIRD_PARTY_NOTICES.txt' 'Third-party notice bundle'
$manifestPath = Require-PackageFile 'build_features.json' 'build_features.json manifest'

try {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
} catch {
    throw "build_features.json is not valid JSON: $($_.Exception.Message)"
}
if ($manifest.schema_version -ne 1) { throw 'build_features.json schema_version must be 1' }
if ($manifest.toolchain.compiler.id -ne 'MSVC') {
    throw "MSVC compiler is required; manifest reports '$($manifest.toolchain.compiler.id)'"
}
foreach ($required in @(
    @('configuration', $manifest.configuration),
    @('source_revision', $manifest.source_revision),
    @('toolchain.compiler.version', $manifest.toolchain.compiler.version),
    @('toolchain.msvc_tools', $manifest.toolchain.msvc_tools),
    @('toolchain.windows_sdk', $manifest.toolchain.windows_sdk),
    @('toolchain.vulkan_sdk', $manifest.toolchain.vulkan_sdk),
    @('dependencies', $manifest.dependencies),
    @('features', $manifest.features),
    @('files', $manifest.files))) {
    if ($null -eq $required[1] -or [string]::IsNullOrWhiteSpace([string]$required[1])) {
        throw "build_features.json is missing $($required[0])"
    }
}
if ($manifest.crt -ne 'static') { throw "static MSVC CRT required; manifest reports '$($manifest.crt)'" }

$manifestFiles = @($manifest.files.psobject.Properties)
if ($manifestFiles.Count -eq 0) { throw 'build_features.json files map is empty' }
foreach ($entry in $manifestFiles) {
    $relative = $entry.Name.Replace('/', [IO.Path]::DirectorySeparatorChar)
    $path = Join-Path $dist $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "manifest file is missing: $($entry.Name)"
    }
    $actual = Get-Sha256Lower $path
    if ($actual -ne ([string]$entry.Value).ToLowerInvariant()) {
        throw "manifest hash mismatch for $($entry.Name): expected $($entry.Value), actual $actual"
    }
}

$forbiddenImports = '^(libstdc\+\+|libgcc|libwinpthread|opengl32).*\.dll$'
$systemDirectory = Join-Path $env:SystemRoot 'System32'
$pending = New-Object 'System.Collections.Generic.Queue[string]'
$pending.Enqueue($editor)
$visited = @{}
$allImports = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
while ($pending.Count -gt 0) {
    $binary = $pending.Dequeue()
    if ($visited.ContainsKey($binary)) { continue }
    $visited[$binary] = $true
    foreach ($import in (Invoke-DumpbinDependents $binary)) {
        [void]$allImports.Add($import)
        if ($import -match $forbiddenImports) {
            throw "forbidden GNU/OpenGL import '$import' in $([IO.Path]::GetFileName($binary))"
        }
        $staged = Join-Path $dist $import
        if (Test-Path -LiteralPath $staged -PathType Leaf) {
            $pending.Enqueue((Resolve-Path -LiteralPath $staged).Path)
            continue
        }
        $system = Join-Path $systemDirectory $import
        if (-not (Test-Path -LiteralPath $system -PathType Leaf)) {
            throw "required runtime DLL '$import' is missing from the package and Windows System32; developer-PATH-only dependencies are forbidden"
        }
    }
}

if ((Get-Item -LiteralPath $notice).Length -eq 0) { throw 'Third-party notice bundle is empty' }
Assert-CleanPathLaunch $editor
Write-Output ("MSVC package: PASS ({0} files, {1} import(s), clean-PATH launch)" -f $manifestFiles.Count, $allImports.Count)
