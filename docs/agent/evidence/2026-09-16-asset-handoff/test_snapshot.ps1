[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Snapshot)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $Snapshot).Path
& (Join-Path $root 'verify-runtime.ps1')
$out = Join-Path $root 'validation'
[void](New-Item -ItemType Directory -Path $out -Force)
$editor = Join-Path $root 'editor.exe'
$dumpbin = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe'
if (-not (Test-Path -LiteralPath $dumpbin)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $dumpbin = Join-Path $vs[0] 'VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe'
}
$imports = @(& $dumpbin /dependents $editor)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect frozen executable imports' }
$imports | Set-Content -LiteralPath (Join-Path $out 'imports.txt')
$dlls = @($imports | ForEach-Object { if ($_ -match '^\s*([A-Za-z0-9_.+\-]+\.dll)\s*$') { $matches[1] } })
foreach ($dll in $dlls) {
    if ($dll -match '^(libstdc\+\+|libgcc|libwinpthread|opengl32|vcruntime|msvcp|ucrtbase)') {
        throw "Unexpected non-static or legacy runtime import: $dll"
    }
    if (-not (Test-Path -LiteralPath (Join-Path "$env:SystemRoot\System32" $dll))) {
        throw "Frozen runtime has an unstaged dependency: $dll"
    }
}
$results = @()
foreach ($mode in @('census','walls-authoring')) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $editor
    $info.WorkingDirectory = $root
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($key in @($info.EnvironmentVariables.Keys)) {
        if ($key -like 'MATTER_*') { $info.EnvironmentVariables.Remove($key) }
    }
    $info.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32"
    $info.EnvironmentVariables['TMP'] = "$env:LOCALAPPDATA\Temp"
    $info.EnvironmentVariables['TEMP'] = "$env:LOCALAPPDATA\Temp"
    $info.EnvironmentVariables['MATTER_ISSUE_DIR'] = Join-Path $root 'issues'
    if ($mode -eq 'census') {
        $info.EnvironmentVariables['MATTER_REGISTRATION_CENSUS'] = '1'
    } else {
        $info.EnvironmentVariables['MATTER_WORLD'] = 'ClayBrickWallSurfaceProof'
        $info.EnvironmentVariables['MATTER_HIDE_WINDOW'] = '1'
        $info.EnvironmentVariables['MATTER_HIDE_UI'] = '1'
        $info.EnvironmentVariables['MATTER_WINDOW_WIDTH'] = '1000'
        $info.EnvironmentVariables['MATTER_WINDOW_HEIGHT'] = '700'
        $info.EnvironmentVariables['MATTER_VT_PROP_TEXELS_PER_METER'] = '512'
        $info.EnvironmentVariables['MATTER_VT_CHART_LOG'] = '1'
        $info.EnvironmentVariables['MATTER_SCREENSHOT'] = Join-Path $out 'walls-authoring.png'
        if (Test-Path -LiteralPath $info.EnvironmentVariables['MATTER_SCREENSHOT']) { throw 'Choose a new capture path; stale screenshots are not accepted' }
        $info.EnvironmentVariables['MATTER_SCREENSHOT_SETTLE'] = '180'
        $info.EnvironmentVariables['MATTER_VK_VALIDATION'] = '1'
    }
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $info
    if (-not $process.Start()) { throw "Cannot start snapshot $mode" }
    Write-Output "Started snapshot $mode process $($process.Id)"
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (-not $process.WaitForExit(1000)) {
        if ($timer.Elapsed.TotalSeconds -gt 300) {
            $process.Kill();$process.WaitForExit()
            throw "Snapshot $mode exceeded its 300-second test deadline"
        }
    }
    $log = $stdout.Result + "`n" + $stderr.Result
    $log | Set-Content -LiteralPath (Join-Path $out "$mode.log")
    if ($process.ExitCode -ne 0) { throw "Snapshot $mode exited $($process.ExitCode)" }
    if ($mode -eq 'census' -and $log -notmatch 'MATTER_REGISTRATION_CENSUS_JSON=\{') {
        throw 'Frozen runtime did not reach registration census'
    }
    if ($mode -eq 'walls-authoring') {
        if (-not (Test-Path -LiteralPath (Join-Path $out 'walls-authoring.png'))) { throw 'Missing snapshot wall capture' }
        if ($log -match '(?i)Validation Error|VUID-|device lost|bake failed|FATAL:') {
            throw 'Frozen wall launch reported a rendering/bake failure; inspect walls.log'
        }
    }
    $results += [ordered]@{mode=$mode;exit=$process.ExitCode;seconds=$timer.Elapsed.TotalSeconds;clean_path=$true}
    $process.Dispose()
    Write-Output "Snapshot $mode passed"
}
$results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'launch-results.json')
