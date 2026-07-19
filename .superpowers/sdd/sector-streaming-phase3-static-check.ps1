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

function Remove-CppComments([string]$text) {
    return [regex]::Replace($text, '(?s)/\*.*?\*/|//[^\r\n]*', '')
}

function Get-CppFunctionBody([string]$text, [string]$signaturePattern) {
    $code = Remove-CppComments $text
    $match = [regex]::Match($code, $signaturePattern)
    if (-not $match.Success) { return '' }
    $open = $code.IndexOf('{', $match.Index)
    if ($open -lt 0) { return '' }
    $depth = 0
    for ($index = $open; $index -lt $code.Length; ++$index) {
        if ($code[$index] -eq '{') { ++$depth }
        elseif ($code[$index] -eq '}') {
            --$depth
            if ($depth -eq 0) { return $code.Substring($open, $index - $open + 1) }
        }
    }
    return ''
}

function Test-FunctionBodyRegex([string]$text, [string]$signaturePattern,
                                [string]$bodyPattern) {
    $body = Get-CppFunctionBody $text $signaturePattern
    return $body.Length -gt 0 -and $body -match $bodyPattern
}

function Require-FunctionBodyRegex([string]$label, [string]$text,
                                   [string]$signaturePattern,
                                   [string]$bodyPattern) {
    if (-not (Test-FunctionBodyRegex $text $signaturePattern $bodyPattern)) {
        $failures.Add("$label missing required statement in its implementation body")
    }
}

function Require-FunctionBodyOrder([string]$label, [string]$text,
                                   [string]$signaturePattern,
                                   [string[]]$patterns) {
    $body = Get-CppFunctionBody $text $signaturePattern
    $cursor = -1
    foreach ($pattern in $patterns) {
        $match = [regex]::Match($body, $pattern)
        if (-not $match.Success -or $match.Index -le $cursor) {
            $failures.Add("$label missing ordered implementation contract /$pattern/")
            return
        }
        $cursor = $match.Index
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
$viewerUiHeader = Read-RepoFile 'MatterViewer\ui.h'
$viewerMain = Read-RepoFile 'MatterViewer\main.cpp'
$session = Read-RepoFile 'MatterEngine3\src\matter_engine.cpp'
$coordinatorHeader = Read-RepoFile 'MatterEngine3\src\streaming\sector_streaming_coordinator.h'
$coordinatorSource = Read-RepoFile 'MatterEngine3\src\streaming\sector_streaming_coordinator.cpp'
$streamerSource = Read-RepoFile 'MatterEngine3\src\sector_streamer.cpp'
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
Require-Count 'Viewer Linux ImGuizmo source union' (Get-AssignmentBlock $viewer 'IMGUI_SRC_LINUX') '$(IMGUIZMO_SRC)' 1
Require-Count 'Viewer Windows ImGuizmo source union' (Get-AssignmentBlock $viewer 'IMGUI_SRC_WIN') '$(IMGUIZMO_SRC)' 1
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
Require-Regex 'Viewer C++ vpath includes streaming sources' $viewer `
    '(?ms)^vpath\s+%\.cpp[^\r\n]*(?:\\\r?\n[^\r\n]*)*\$\(ME3_DIR\)/src/streaming'

# Hover capture is meaningful only when this frame submitted the detached
# translation gizmo. Keep IsUsing unconditional so an in-progress operation
# remains safe even if the next panel path does not submit a new gizmo.
Require-Regex 'Viewer per-frame gizmo submission state' $viewerUiHeader '(?m)^\s*bool\s+gizmo_submitted_\s*=\s*false\s*;'
Require-FunctionBodyRegex 'Viewer frame resets gizmo submission state' $viewerUi '(?s)\bbool\s+Ui::begin_frame\s*\([^)]*\)\s*\{' 'gizmo_submitted_\s*=\s*false\s*;'
Require-FunctionBodyRegex 'Viewer gizmo submission precedes manipulation' $viewerUi '(?s)\bvoid\s+Ui::draw_sector_streaming_panel\s*\([^)]*\)\s*\{' 'gizmo_submitted_\s*=\s*true\s*;\s*if\s*\(\s*ImGuizmo::Manipulate\s*\('
Require-Count 'Viewer gizmo retains BeginFrame full-screen drawlist' `
    (Get-CppFunctionBody $viewerUi '(?s)\bvoid\s+Ui::draw_sector_streaming_panel\s*\([^)]*\)\s*\{') `
    'ImGuizmo::SetDrawlist()' 0
Require-FunctionBodyRegex 'Viewer hover capture requires this-frame gizmo submission' $viewerUi '(?s)\bbool\s+Ui::camera_input_allowed\s*\(\s*\)\s*const\s*\{' 'gizmo_submitted_\s*&&\s*ImGuizmo::IsOver\s*\(\s*ImGuizmo::TRANSLATE\s*\)'
Require-FunctionBodyRegex 'Viewer capture retains safe gizmo use query' $viewerUi '(?s)\bbool\s+Ui::camera_input_allowed\s*\(\s*\)\s*const\s*\{' 'ImGuizmo::IsUsing\s*\(\s*\)'

# Bake focus remains a closed-world ordering/refinement API only; the Vulkan
# Viewer must not drive streaming through a per-frame focus update.
Require-Count 'MatterViewer per-frame bake focus' $viewerMain 'set_bake_focus' 0

# Preserve Task 4's durable admission and lifecycle boundaries with scoped,
# comment-stripped implementation contracts rather than behavioral re-runs.
$coordinatorNextRequest = '(?s)\bbool\s+Coordinator::next_request\s*\(\s*TaggedRequest&\s+out,\s*void\*\s+fault_context,\s*RequestTrackingFault\s+fault\s*\)\s*noexcept\s*\{'
$streamerCancelRequest = '(?s)\bbool\s+SectorStreamer::cancel_request\s*\(\s*int64_t\s+tx,\s*int64_t\s+tz,\s*int\s+rung\s*\)\s*noexcept\s*\{'
$streamStep = '(?s)\bvoid\s+WorldSession::Impl::execute_sector_stream_step\s*\(\s*\)\s*\{'
$clearProfile = '(?s)\bbool\s+WorldSession::Impl::clear_streaming_profile\s*\(\s*std::string&\s+err,\s*bool\s+restore_on_failure\s*\)\s*\{'

# Checker fixtures prove a comment cannot satisfy the body-aware cancellation
# contract, while an executable statement in the intended body can.
$commentOnlyFixture = 'bool Coordinator::next_request(TaggedRequest& out, void* fault_context, RequestTrackingFault fault) noexcept { // streamer_->cancel_request(sector.tx, sector.tz, sector.rung); return false; }'
$liveFixture = 'bool Coordinator::next_request(TaggedRequest& out, void* fault_context, RequestTrackingFault fault) noexcept { streamer_->cancel_request(sector.tx, sector.tz, sector.rung); return false; }'
$commentOnlyPasses = Test-FunctionBodyRegex $commentOnlyFixture $coordinatorNextRequest 'streamer_->cancel_request\s*\('
$liveFixturePasses = Test-FunctionBodyRegex $liveFixture $coordinatorNextRequest 'streamer_->cancel_request\s*\('
if ($commentOnlyPasses -or -not $liveFixturePasses) {
    $failures.Add('Phase 3 checker self-test failed: comments must not satisfy lifecycle contracts')
}

Require-Regex 'durable completion capacity declaration' $coordinatorHeader '(?m)^class\s+PublicationCompletionCapacity\s*\{'
Require-FunctionBodyRegex 'non-allocating admission reserve' $coordinatorSource '(?s)\bbool\s+PublicationCompletionCapacity::try_reserve\s*\(\s*size_t&\s+slot\s*\)\s*noexcept\s*\{' 'occupied_\[index\]\s*=\s*true'
Require-FunctionBodyRegex 'completion release' $coordinatorSource '(?s)\bvoid\s+PublicationCompletionCapacity::release\s*\(\s*size_t\s+slot\s*\)\s*noexcept\s*\{' 'occupied_\[slot\]\s*=\s*false'
Require-Regex 'strong request tracking declaration' $coordinatorHeader '(?m)^enum class\s+RequestTrackingStage\s*:\s*uint8_t\s*\{'
Require-Regex 'noexcept request admission declaration' $coordinatorHeader '(?ms)^\s*bool\s+next_request\s*\(.*?RequestTrackingFault\s+fault\s*=\s*nullptr\)\s*noexcept\s*;'
Require-FunctionBodyRegex 'Coordinator request admission reserves before streamer mutation' $coordinatorSource $coordinatorNextRequest 'issued_requests_\.reserve[\s\S]*publication_candidates_\.reserve[\s\S]*streamer_->next_request'
Require-FunctionBodyRegex 'Coordinator exact request rollback' $coordinatorSource $coordinatorNextRequest 'streamer_->cancel_request\s*\(\s*sector\.tx,\s*sector\.tz,\s*sector\.rung\s*\)'
Require-FunctionBodyRegex 'SectorStreamer exact cancellation clears matching inflight rung' $streamerSource $streamerCancelRequest 'inflight_rung\s*=\s*-1[\s\S]*--inflight_'
Require-Regex 'durable eviction retention declaration' $coordinatorHeader '(?m)^class\s+PendingEvictionBatch\s*\{'
Require-Regex 'profile lifecycle gate declaration' $coordinatorHeader '(?m)^class\s+ProfileActivationGate\s*\{'
Require-FunctionBodyOrder 'session stream admission' $session $streamStep @(
    'reserve_publication_completion\s*\(',
    'coordinator\.next_request\s*\(',
    'begin_publication\s*\(')
Require-FunctionBodyOrder 'session lifecycle clear barrier' $session $clearProfile @(
    'streaming_profile_activation\.begin_clear\s*\(',
    'coordinator\.worker_step\s*\(',
    'drain_sector_evictions\s*\(\s*true',
    'streaming_profile_activation\.finish_clear\s*\(')
Require-FunctionBodyRegex 'single app eviction helper' $session '(?s)\bbool\s+WorldSession::Impl::apply_sector_evictions\s*\(' 'matter_async::assert_gl_thread\s*\(\s*"stream\.apply_evictions"\s*\)'

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
    if ($entry.Text -match '(?i)ExplorerDemo[\\/]|\bExplorerDemo\b.*\b(?:build|runtime|package|smoke)|\b(?:build|runtime|package|smoke)\b.*\bExplorerDemo\b') {
        $failures.Add("$($entry.Label) retains an Explorer path or build/runtime/package/smoke expectation")
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
