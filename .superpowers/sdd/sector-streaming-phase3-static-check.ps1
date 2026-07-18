$ErrorActionPreference = 'Stop'

# This audit is intentionally source-only: it closes the Phase 3 build and
# ownership seams without launching the retired app or any GPU automation.
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$failures = [System.Collections.Generic.List[string]]::new()

function Read-RepoFile([string]$relativePath) {
    $path = Join-Path $root $relativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $failures.Add("missing file '$relativePath'")
        return ''
    }
    return [System.IO.File]::ReadAllText($path)
}

function Get-AssignmentBlock([string]$text, [string]$name) {
    $lines = $text -split "`r?`n"
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -notmatch ('^\s*' + [regex]::Escape($name) + '\s*[:+?]?=')) {
            continue
        }
        $block = [System.Collections.Generic.List[string]]::new()
        for ($j = $i; $j -lt $lines.Count; ++$j) {
            $block.Add($lines[$j])
            if ($lines[$j].TrimEnd() -notmatch '\\$') { break }
        }
        return $block -join "`n"
    }
    return ''
}

function Get-AssignmentNamesContaining([string]$text, [string]$needle) {
    $names = [System.Collections.Generic.List[string]]::new()
    foreach ($match in [regex]::Matches($text, '(?m)^\s*([A-Za-z0-9_]+)\s*[:+?]?=')) {
        $name = $match.Groups[1].Value
        if ((Get-AssignmentBlock $text $name).Contains($needle)) { $names.Add($name) }
    }
    return $names
}

function Get-LiteralSources([string]$text, [string[]]$variables) {
    $sources = [System.Collections.Generic.List[string]]::new()
    foreach ($variable in $variables) {
        foreach ($match in [regex]::Matches(
            (Get-AssignmentBlock $text $variable), '[^\s\\]+?\.(?:cpp|c)(?=\s|$)')) {
            $sources.Add($match.Value)
        }
    }
    return $sources
}

function Require-Contains([string]$label, [string]$text, [string]$needle) {
    if (-not $text.Contains($needle)) { $failures.Add("$label missing '$needle'") }
}

function Require-Regex([string]$label, [string]$text, [string]$pattern) {
    if ($text -notmatch $pattern) { $failures.Add("$label did not match /$pattern/") }
}

function Require-Count([string]$label, [string]$text, [string]$needle, [int]$expected) {
    $actual = ([regex]::Matches($text, [regex]::Escape($needle))).Count
    if ($actual -ne $expected) {
        $failures.Add("$label expected $expected occurrence(s) of '$needle', found $actual")
    }
}

function Require-UniqueBasenames([string]$label, [string[]]$sources) {
    $basenames = foreach ($source in $sources) {
        [System.IO.Path]::GetFileNameWithoutExtension(($source -replace '/', '\\'))
    }
    $duplicates = @($basenames | Group-Object | Where-Object Count -gt 1 |
        ForEach-Object Name)
    if ($duplicates.Count -gt 0) {
        $failures.Add("$label basename collision(s): $($duplicates -join ', ')")
    }
}

$streamingHeader = Read-RepoFile 'MatterEngine3\include\matter\streaming.h'
$engine = Read-RepoFile 'MatterEngine3\Makefile'
$tests = Read-RepoFile 'MatterEngine3\tests\Makefile'
$viewer = Read-RepoFile 'MatterViewer\Makefile'
$viewerUi = Read-RepoFile 'MatterViewer\ui.cpp'
$viewerMain = Read-RepoFile 'MatterViewer\main.cpp'
$session = Read-RepoFile 'MatterEngine3\src\matter_engine.cpp'
$coordinatorHeader = Read-RepoFile 'MatterEngine3\src\streaming\sector_streaming_coordinator.h'
$coordinatorSource = Read-RepoFile 'MatterEngine3\src\streaming\sector_streaming_coordinator.cpp'
$buildAll = Read-RepoFile 'build-all.sh'
$gitignore = Read-RepoFile '.gitignore'

# Public streaming remains data-only: coordination and worker ownership are private.
foreach ($type in @('SectorStreaming', 'SectorStreamingError', 'SectorStreamingStatus',
                    'StreamingUpdate', 'StreamingModule')) {
    Require-Regex "public streaming type $type" $streamingHeader ("\b(?:struct|class)\s+" + $type + "\b")
}
foreach ($match in [regex]::Matches($streamingHeader, '(?s)(?:struct|class)\s+\w+\s*\{(.*?)\};')) {
    if ($match.Value -match '(?:\b(?:Coordinator|SectorStreamer|Renderer|Worker)\b|\b(?:std::)?mutex\b|\b(?:std::)?(?:unique_ptr|shared_ptr)\b|\w+\s*\*)') {
        $failures.Add('public streaming.h exposes private coordinator/streamer/render/worker/mutex/pointer storage')
        break
    }
}

# Discover every literal source union which owns Runtime, then close its complete
# streaming group exactly once. This catches future test/application graphs too.
foreach ($entry in @(
    @{ Label = 'engine'; Text = $engine; Runtime = 'src/ecs/ecs_runtime.cpp'; Prefix = 'src/' },
    @{ Label = 'tests'; Text = $tests; Runtime = '../src/ecs/ecs_runtime.cpp'; Prefix = '../src/' },
    @{ Label = 'Viewer'; Text = $viewer; Runtime = '$(ME3_DIR)/src/ecs/ecs_runtime.cpp'; Prefix = '$(ME3_DIR)/src/' }
)) {
    $owners = @(Get-AssignmentNamesContaining $entry.Text $entry.Runtime)
    if ($owners.Count -eq 0) { $failures.Add("$($entry.Label) has no Runtime-bearing source graph") }
    foreach ($owner in $owners) {
        $block = Get-AssignmentBlock $entry.Text $owner
        Require-Count "$($entry.Label) $owner Runtime" $block $entry.Runtime 1
        Require-Count "$($entry.Label) $owner ECS streaming" $block ($entry.Prefix + 'ecs/streaming_systems.cpp') 1
        Require-Count "$($entry.Label) $owner coordinator" $block ($entry.Prefix + 'streaming/sector_streaming_coordinator.cpp') 1
    }
}

# MatterViewer owns exactly one controller TU in each platform app graph, and
# both platform ImGui source unions consume the pinned ImGuizmo TU once.
foreach ($appGraph in @('LINUX_APP_SRC', 'APP_SRC')) {
    Require-Count "Viewer $appGraph controller" (Get-AssignmentBlock $viewer $appGraph) 'streaming_anchor_controller.cpp' 1
}
Require-Count 'Viewer ImGuizmo include' $viewerUi '#include "ImGuizmo.h"' 1
Require-Count 'Viewer ImGuizmo path include' (Get-AssignmentBlock $viewer 'INCLUDE_PATHS') '-I$(IMGUIZMO_PATH)' 1
Require-Count 'Viewer ImGuizmo source' (Get-AssignmentBlock $viewer 'IMGUIZMO_SRC') '$(IMGUIZMO_PATH)/ImGuizmo.cpp' 1
Require-Count 'Viewer pinned ImGuizmo commit' $viewer 'dc25afb98bc3ebe00dfc9a23ba7235fead2ccb1d' 1
$upstream = Read-RepoFile 'Libraries\ImGuizmo\UPSTREAM.md'
$license = Read-RepoFile 'Libraries\ImGuizmo\LICENSE'
Require-Count 'ImGuizmo upstream pin' $upstream 'dc25afb98bc3ebe00dfc9a23ba7235fead2ccb1d' 1
Require-Contains 'ImGuizmo license' $license 'The MIT License (MIT)'
[void](Read-RepoFile 'Libraries\ImGuizmo\ImGuizmo.h')
[void](Read-RepoFile 'Libraries\ImGuizmo\ImGuizmo.cpp')
Require-UniqueBasenames 'Viewer flattened Windows source union' @(
    Get-LiteralSources $viewer @('APP_SRC', 'WIN_ME3_CPP', 'WIN_MSL_CPP',
        'IMGUI_CORE_SRC', 'IMGUIZMO_SRC', 'IMGUI_SRC_WIN', 'WIN_PIPELINE_C',
        'QJS_C', 'FLECS_C'))

# Bake focus remains a closed-world ordering/refinement API only; the Vulkan
# Viewer must not drive streaming through a per-frame focus update.
Require-Count 'MatterViewer per-frame bake focus' $viewerMain 'set_bake_focus' 0

# Preserve Task 4's durable admission and lifecycle boundaries with source
# contracts rather than re-running the behavioral fake-endpoint suite here.
foreach ($contract in @(
    @{ Label = 'durable completion capacity'; Text = $coordinatorHeader; Needle = 'class PublicationCompletionCapacity' },
    @{ Label = 'non-allocating admission reserve'; Text = $coordinatorSource; Needle = 'PublicationCompletionCapacity::try_reserve' },
    @{ Label = 'completion release'; Text = $coordinatorSource; Needle = 'PublicationCompletionCapacity::release' },
    @{ Label = 'strong request tracking'; Text = $coordinatorHeader; Needle = 'RequestTrackingStage' },
    @{ Label = 'noexcept request admission'; Text = $coordinatorHeader; Needle = 'RequestTrackingFault fault = nullptr) noexcept;' },
    @{ Label = 'exact request cancellation'; Text = $coordinatorSource; Needle = 'streamer_->cancel_request(' },
    @{ Label = 'durable eviction retention'; Text = $coordinatorHeader; Needle = 'class PendingEvictionBatch' },
    @{ Label = 'profile lifecycle gate'; Text = $coordinatorHeader; Needle = 'class ProfileActivationGate' },
    @{ Label = 'single app eviction helper'; Text = $session; Needle = 'WorldSession::Impl::apply_sector_evictions(' },
    @{ Label = 'publication transaction'; Text = $session; Needle = 'streaming::detail::PublicationTransaction transaction(' }
)) {
    Require-Contains $contract.Label $contract.Text $contract.Needle
}

# Retirement checks use the tracked index (not filesystem state) and search only
# active automation for build/runtime/package/smoke expectations. Historical docs
# are deliberately outside this scope.
$trackedExplorer = @(& git -C $root ls-files -- ExplorerDemo 2>$null)
if ($LASTEXITCODE -ne 0) {
    $failures.Add('could not query tracked ExplorerDemo files with git ls-files')
} elseif ($trackedExplorer.Count -ne 0) {
    $failures.Add("tracked ExplorerDemo files remain: $($trackedExplorer -join ', ')")
}
$activeAutomation = @(
    @{ Label = 'build-all.sh'; Text = $buildAll },
    @{ Label = '.gitignore'; Text = $gitignore }
)
foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -File |
         Where-Object { $_.Name -match '(?i)(?:check|gate).*\.(?:ps1|sh)$' }) {
    if ($file.FullName -eq $PSCommandPath) { continue }
    $relative = $file.FullName.Substring($root.Length + 1)
    $activeAutomation += @{ Label = $relative; Text = [System.IO.File]::ReadAllText($file.FullName) }
}
foreach ($entry in $activeAutomation) {
    if ($entry.Text -match '(?im)ExplorerDemo/(?:explorer|cache|shaders?|dist)|\bExplorerDemo\b.*\b(?:build|runtime|package|smoke)|\b(?:build|runtime|package|smoke)\b.*\bExplorerDemo\b') {
        $failures.Add("$($entry.Label) retains an Explorer build/runtime/package/smoke expectation")
    }
}

if ($failures.Count -gt 0) {
    Write-Host "FAIL: Sector streaming Phase 3 closure ($($failures.Count) issue(s))"
    foreach ($failure in $failures) { Write-Host " - $failure" }
    exit 1
}

Write-Host 'PASS: Sector streaming Phase 3 closure'
Write-Host ' - public streaming remains data-only and Runtime graphs close exactly once'
Write-Host ' - Viewer controller and pinned ImGuizmo source graphs are complete and collision-free'
Write-Host ' - durable Task 4 admission/lifecycle contracts and Explorer retirement are intact'
