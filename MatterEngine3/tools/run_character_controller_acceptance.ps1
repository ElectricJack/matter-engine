[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 4200
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-JsonFile($Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 12) + "`n",
        [Text.UTF8Encoding]::new($false))
}
function Get-Sha($Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Get-TimeNs { [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() * [long]1000000 }
function Copy-Tree($Source, $Destination) {
    # Never follow a link into a live project or alias cache storage.
    $entries = @(Get-ChildItem -LiteralPath $Source -Force -Recurse)
    if ($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
        throw "Fixture input contains a reparse point: $Source"
    }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Get-ChildItem -LiteralPath $Source -Force |
        Copy-Item -Destination $Destination -Recurse
}
function Add-Steps($Lines, [int]$Count) {
    for ($i = 0; $i -lt $Count; ++$i) {
        $Lines.Add('step')
        $Lines.Add('wait_frames 1')
    }
}
function Add-Sample($Lines, $RunRoot, $Label, [bool]$Capture = $true) {
    $Lines.Add("character status $Label")
    if ($Capture) { $Lines.Add('shot_now ' + (Join-Path $RunRoot "$Label.png").Replace('\', '/')) }
}
function Write-Timeline($RunRoot) {
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('wait_event bake.finished 3600')
    $lines.Add('wait_idle 120')
    # Keep a ravine/horizon view while walking's post-tick eye follows the capsule.
    $lines.Add('cam 48 91 31 16 86 7')
    Add-Sample $lines $RunRoot 'edit'
    $lines.Add('character walk on')
    $lines.Add('pause')
    $lines.Add('character intent 0 0 0')
    Add-Steps $lines 300
    Add-Sample $lines $RunRoot 'grounded'
    $lines.Add('character intent 1 0 0')
    Add-Steps $lines 60
    Add-Sample $lines $RunRoot 'walk'
    $lines.Add('character intent 1 0 1')
    Add-Steps $lines 30
    Add-Sample $lines $RunRoot 'sprint'
    $lines.Add('character intent 0 0 0')
    Add-Steps $lines 120
    Add-Sample $lines $RunRoot 'jump_base' $false
    $lines.Add('character jump')
    Add-Sample $lines $RunRoot 'latched' $false
    $lines.Add('wait_frames 5')
    Add-Sample $lines $RunRoot 'paused' $false
    Add-Steps $lines 1
    Add-Sample $lines $RunRoot 'jump'
    Add-Steps $lines 120
    Add-Sample $lines $RunRoot 'landed'
    $lines.Add('play')
    $lines.Add('wait_frames 20')
    $lines.Add('pause')
    Add-Sample $lines $RunRoot 'resumed' $false
    $lines.Add('wait_frames 5')
    Add-Sample $lines $RunRoot 'paused_again' $false
    $lines.Add('character jump')
    $lines.Add('sim stop')
    $lines.Add('wait_frames 2')
    Add-Sample $lines $RunRoot 'stopped'
    $lines.Add('cam 48 91 31 31 69 7')
    $lines.Add('shot_now ' + (Join-Path $RunRoot 'river-restored.png').Replace('\', '/'))
    $lines.Add('play')
    $lines.Add('wait_frames 120')
    $lines.Add('pause')
    $lines.Add('shot_now ' + (Join-Path $RunRoot 'river-play.png').Replace('\', '/'))
    $lines.Add('quit')
    $path = Join-Path $RunRoot 'timeline.txt'
    [IO.File]::WriteAllText($path, ($lines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
    return $path
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$outputRoot = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $outputRoot) {
    if (-not (Test-Path -LiteralPath $outputRoot -PathType Container) -or
        (Get-Item -LiteralPath $outputRoot).Attributes -band [IO.FileAttributes]::ReparsePoint -or
        @(Get-ChildItem -LiteralPath $outputRoot -Force).Count -ne 0) {
        throw "Output must be a new or empty real directory: $outputRoot"
    }
}
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $outputRoot).Path
$savedMatter = @{}
Get-ChildItem Env:MATTER_* | ForEach-Object { $savedMatter[$_.Name] = $_.Value }
$savedPath = $env:PATH
try {
    if (@(Get-Process editor -ErrorAction SilentlyContinue).Count) {
        throw 'An editor is running; refusing to build or copy a potentially live cache.'
    }
    $buildRoot = Join-Path $repoRoot 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo'
    $cachePath = Join-Path $buildRoot 'CMakeCache.txt'
    $cache = Get-Content -LiteralPath $cachePath -Raw
    $feature = [regex]::Matches($cache, '(?m)^MATTER_ENABLE_PHYSX:BOOL=(.*)\r?$')
    if ($feature.Count -ne 1 -or $feature[0].Groups[1].Value.Trim() -cne 'ON') {
        throw 'Acceptance requires exactly MATTER_ENABLE_PHYSX:BOOL=ON'
    }
    $rootEntry = [regex]::Matches($cache, '(?m)^MATTER_PHYSX_ROOT:PATH=(.*)\r?$')
    if ($rootEntry.Count -ne 1) { throw 'Missing/ambiguous pinned PhysX CMake root' }
    $physxRoot = (Resolve-Path -LiteralPath $rootEntry[0].Groups[1].Value.Trim()).Path.TrimEnd('\', '/')
    if ($env:MATTER_PHYSX_ROOT) {
        $environmentRoot = (Resolve-Path -LiteralPath $env:MATTER_PHYSX_ROOT).Path.TrimEnd('\', '/')
        if ($environmentRoot -ine $physxRoot) { throw 'Environment and CMake PhysX roots conflict' }
    }
    $pinnedDll = Join-Path $physxRoot 'physx/bin/win.x86_64.vc143.mt/release/PhysXGpu_64.dll'
    if (-not (Test-Path -LiteralPath $pinnedDll -PathType Leaf)) { throw "Missing pinned GPU DLL: $pinnedDll" }
    $wrapper = Join-Path $repoRoot 'tools/build-windows.ps1'
    $toolchain = (& $wrapper -EnablePhysx -PhysxRoot $physxRoot -PreflightOnly | Out-String) | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { throw 'Native preflight failed' }
    # An up-to-date build can be a no-op; invoking the graph is the freshness proof.
    & $wrapper -Config RelWithDebInfo -EnablePhysx -PhysxRoot $physxRoot -Target matter_editor 2>&1 |
        Tee-Object -FilePath (Join-Path $outputRoot 'fresh-build.log')
    if ($LASTEXITCODE -ne 0) { throw 'Fresh native PhysX editor build failed' }
    $cache = Get-Content -LiteralPath $cachePath -Raw
    if ($cache -notmatch '(?m)^MATTER_ENABLE_PHYSX:BOOL=ON\r?$') { throw 'Build disabled PhysX' }
    $sourceEditor = Join-Path $repoRoot 'MatterEditor/build/windows-msvc/editor.exe'
    if (-not (Test-Path -LiteralPath $sourceEditor -PathType Leaf)) { throw 'Missing MSVC editor' }
    $fixture = Join-Path $outputRoot 'f'
    $fixtureBin = Join-Path $fixture 'bin'
    $fixtureTools = Join-Path $fixture 'MatterEngine3/tools'
    New-Item -ItemType Directory -Path $fixtureBin, $fixtureTools, (Join-Path $fixture 'MatterEditor') -Force | Out-Null
    Copy-Tree (Join-Path $repoRoot 'projects/world_demo') (Join-Path $fixture 'projects/world_demo')
    Copy-Tree (Join-Path $repoRoot 'MatterEngine3/shared-lib') (Join-Path $fixture 'MatterEngine3/shared-lib')
    $fixtureEditor = Join-Path $fixtureBin 'editor.exe'
    Copy-Item -LiteralPath $sourceEditor -Destination $fixtureEditor
    foreach ($directory in @($buildRoot, (Split-Path $sourceEditor))) {
        Get-ChildItem -LiteralPath $directory -Filter '*.dll' |
            Copy-Item -Destination $fixtureBin -Force
    }
    $copiedDll = Join-Path $fixtureBin 'PhysXGpu_64.dll'
    Copy-Item -LiteralPath $pinnedDll -Destination $copiedDll -Force
    $drive = Join-Path $fixtureTools 'drive.py'
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'drive.py') -Destination $drive
    $editorHash = Get-Sha $sourceEditor
    $dllHash = Get-Sha $pinnedDll
    if ($editorHash -ne (Get-Sha $fixtureEditor) -or $dllHash -ne (Get-Sha $copiedDll)) {
        throw 'Source/copy editor or pinned PhysX GPU DLL hash mismatch'
    }
    $sourceHashes = [ordered]@{}
    $scriptFiles = @(Get-ChildItem -LiteralPath (Join-Path $fixture 'projects/world_demo') -Recurse -File -Filter '*.js') +
        @(Get-ChildItem -LiteralPath (Join-Path $fixture 'MatterEngine3/shared-lib') -Recurse -File -Filter '*.js')
    foreach ($file in $scriptFiles) {
        $relativePath = $file.FullName.Substring($fixture.Length + 1)
        $sourceHashes[$relativePath] = Get-Sha (Join-Path $repoRoot $relativePath)
        if ($sourceHashes[$relativePath] -ne (Get-Sha $file.FullName)) {
            throw "Source/copy script mismatch: $relativePath"
        }
    }
    foreach ($file in @('drive.py', 'run_character_controller_acceptance.ps1', 'character_controller_acceptance.py')) {
        $sourceHashes["MatterEngine3/tools/$file"] = Get-Sha (Join-Path $PSScriptRoot $file)
    }
    $provenance = [ordered]@{
        head = (& git -C $repoRoot rev-parse HEAD | Out-String).Trim()
        dirty_files = @(& git -C $repoRoot status --short)
        cmake_physx = 'MATTER_ENABLE_PHYSX:BOOL=ON'; physx_root = $physxRoot
        source_editor = $sourceEditor; source_editor_sha256 = $editorHash
        copied_editor = $fixtureEditor; copied_editor_sha256 = Get-Sha $fixtureEditor
        pinned_dll = $pinnedDll; pinned_dll_sha256 = $dllHash
        copied_dll = $copiedDll; copied_dll_sha256 = Get-Sha $copiedDll
        toolchain = $toolchain; source_script_sha256 = $sourceHashes
        source_cache = Join-Path $repoRoot 'projects/world_demo/.cache'
        fixture_cache = Join-Path $fixture 'projects/world_demo/.cache'
        source_cache_policy = 'read-only copied input; no deletion, invalidation, links, or junctions'
        normal_rendering = $true; validation = $true; width = 1280; height = 720
        controller_camera_eye = @(48, 91, 31); controller_camera_target = @(16, 86, 7)
    }
    Write-JsonFile (Join-Path $outputRoot 'provenance.json') $provenance
    Get-ChildItem Env:MATTER_* | ForEach-Object { Remove-Item -LiteralPath "Env:$($_.Name)" }
    $env:PATH = "$fixtureBin;$savedPath"
    $runs = @()
    foreach ($number in 1, 2) {
        $runRoot = Join-Path $outputRoot "run$number"
        New-Item -ItemType Directory -Path $runRoot | Out-Null
        $timeline = Write-Timeline $runRoot
        $started = Get-TimeNs
        & $toolchain.Python $drive --world RiverFloatLab --timeline $timeline --out-dir $runRoot `
            --timeout $TimeoutSeconds --editor $fixtureEditor --hide-ui `
            --env MATTER_WINDOW_WIDTH=1280 --env MATTER_WINDOW_HEIGHT=720 --env MATTER_VK_VALIDATION=1 2>&1 |
            Tee-Object -FilePath (Join-Path $runRoot 'drive.log')
        $runExit = $LASTEXITCODE
        Write-JsonFile (Join-Path $runRoot 'run.json') @{
            started_ns = $started; finished_ns = Get-TimeNs; drive_exit = $runExit
            world = 'RiverFloatLab'; timeline_sha256 = Get-Sha $timeline
        }
        $runs += $runRoot
        if ($runExit -ne 0) { throw "Native editor run $number failed with exit $runExit; artifacts retained" }
    }
    & $toolchain.Python (Join-Path $PSScriptRoot 'character_controller_acceptance.py') `
        --run $runs[0] --run $runs[1] --output (Join-Path $outputRoot 'summary.json')
    if ($LASTEXITCODE -ne 0) { throw 'Character evidence checker failed; all runs retained' }
    Write-Output "Character acceptance evidence: $outputRoot (visual inspection still required)"
} catch {
    Write-JsonFile (Join-Path $outputRoot 'failure.json') @{ error = $_.ToString(); time = [DateTimeOffset]::UtcNow.ToString('o') }
    throw
} finally {
    Get-ChildItem Env:MATTER_* | ForEach-Object { Remove-Item -LiteralPath "Env:$($_.Name)" }
    foreach ($entry in $savedMatter.GetEnumerator()) { Set-Item -LiteralPath "Env:$($entry.Key)" -Value $entry.Value }
    $env:PATH = $savedPath
}
