$ErrorActionPreference = 'Stop'

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
        if ($lines[$i] -notmatch ('^' + [regex]::Escape($name) + '\s*[:+?]?=')) {
            continue
        }
        $block = [System.Collections.Generic.List[string]]::new()
        for ($j = $i; $j -lt $lines.Count; ++$j) {
            $block.Add($lines[$j])
            if ($lines[$j].TrimEnd() -notmatch '\\$') {
                break
            }
        }
        return $block -join "`n"
    }
    return ''
}

function Get-RuleBlock([string]$text, [string]$targetPattern) {
    $lines = $text -split "`r?`n"
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -notmatch $targetPattern) {
            continue
        }
        $block = [System.Collections.Generic.List[string]]::new()
        for ($j = $i; $j -lt $lines.Count; ++$j) {
            if ($j -gt $i -and $lines[$j] -notmatch '^(\t|\s+\S.*\\$|\s*$)') {
                break
            }
            $block.Add($lines[$j])
            if ($j -gt $i -and $lines[$j] -match '^\s*$') {
                break
            }
        }
        return $block -join "`n"
    }
    return ''
}

function Require-Contains([string]$label, [string]$text, [string]$needle) {
    if (-not $text.Contains($needle)) {
        $failures.Add("$label missing '$needle'")
    }
}

function Require-Regex([string]$label, [string]$text, [string]$pattern) {
    if ($text -notmatch $pattern) {
        $failures.Add("$label did not match /$pattern/")
    }
}

function Require-Count([string]$label, [string]$text, [string]$needle, [int]$expected) {
    $actual = ([regex]::Matches($text, [regex]::Escape($needle))).Count
    if ($actual -ne $expected) {
        $failures.Add("$label expected $expected occurrence(s) of '$needle', found $actual")
    }
}

$engine = Read-RepoFile 'MatterEngine3\Makefile'
$tests = Read-RepoFile 'MatterEngine3\tests\Makefile'
$viewer = Read-RepoFile 'MatterViewer\Makefile'
$explorer = Read-RepoFile 'ExplorerDemo\Makefile'
$runtimeHeader = Read-RepoFile 'MatterEngine3\src\ecs\ecs_runtime.h'
$runtimeSource = Read-RepoFile 'MatterEngine3\src\ecs\ecs_runtime.cpp'
$contextHeader = Read-RepoFile 'MatterEngine3\src\ecs\physics_context.h'
$contextSource = Read-RepoFile 'MatterEngine3\src\ecs\physics_context.cpp'
$publicPhysics = Read-RepoFile 'MatterEngine3\include\matter\physics.h'

# Box3D stays private and Runtime owns its context after the Flecs world member.
Require-Count 'public physics header Box3D includes' $publicPhysics 'box3d/' 0
Require-Regex 'private context implementation Box3D include' $contextSource '#include\s+[<"]box3d/box3d\.h[>"]'
Require-Count 'private context header opaque Box3D storage' $contextHeader 'b3WorldId' 0
Require-Contains 'Runtime out-of-line destructor declaration' $runtimeHeader '~Runtime();'
Require-Regex 'Runtime member destruction order' $runtimeHeader 'flecs::world world_;\s*std::unique_ptr<physics::detail::PhysicsContext> physics_;'
Require-Regex 'Runtime imports PhysicsModule' $runtimeSource 'world_\.import<physics::PhysicsModule>\(\);'
Require-Regex 'Runtime publishes private context ref' $runtimeSource 'world_\.set<physics::detail::PhysicsContextRef>'

# Every literal source list that owns ecs_runtime.cpp must also own exactly one
# physics_context.cpp. This catches focused tests and the shared GPU graph.
foreach ($entry in @(
    @{ Label = 'engine ME3_CPP'; Text = $engine; Name = 'ME3_CPP'; Runtime = 'src/ecs/ecs_runtime.cpp'; Context = 'src/ecs/physics_context.cpp' },
    @{ Label = 'tests ECS_CPP'; Text = $tests; Name = 'ECS_CPP'; Runtime = '../src/ecs/ecs_runtime.cpp'; Context = '../src/ecs/physics_context.cpp' },
    @{ Label = 'tests PHYSICS_CPP'; Text = $tests; Name = 'PHYSICS_CPP'; Runtime = '../src/ecs/ecs_runtime.cpp'; Context = '../src/ecs/physics_context.cpp' },
    @{ Label = 'tests GPU_ALL_CPP'; Text = $tests; Name = 'GPU_ALL_CPP'; Runtime = '../src/ecs/ecs_runtime.cpp'; Context = '../src/ecs/physics_context.cpp' },
    @{ Label = 'viewer WIN_ME3_CPP'; Text = $viewer; Name = 'WIN_ME3_CPP'; Runtime = '$(ME3_DIR)/src/ecs/ecs_runtime.cpp'; Context = '$(ME3_DIR)/src/ecs/physics_context.cpp' },
    @{ Label = 'explorer WIN_ME3_CPP'; Text = $explorer; Name = 'WIN_ME3_CPP'; Runtime = '$(ME3_DIR)/src/ecs/ecs_runtime.cpp'; Context = '$(ME3_DIR)/src/ecs/physics_context.cpp' }
)) {
    $block = Get-AssignmentBlock $entry.Text $entry.Name
    Require-Count "$($entry.Label) Runtime" $block $entry.Runtime 1
    Require-Count "$($entry.Label) PhysicsContext" $block $entry.Context 1
}

# Shared compiler include graphs must all see the pinned Box3D headers.
foreach ($entry in @(
    @{ Label = 'engine'; Text = $engine },
    @{ Label = 'tests'; Text = $tests },
    @{ Label = 'viewer'; Text = $viewer },
    @{ Label = 'explorer'; Text = $explorer }
)) {
    Require-Count "$($entry.Label) shared Box3D include" (Get-AssignmentBlock $entry.Text 'INCLUDE_PATHS') '-I$(BOX3D_DIR)/include' 1
}

# The focused Runtime test binaries link exactly one native Box3D archive.
Require-Regex 'physics target name' $tests '(?m)^PHYSICS_TARGET\s*=\s*physics-tests\s*$'
Require-Count 'removed contract-only target name' $tests 'PHYSICS_CONTRACT' 0
Require-Contains 'run-physics target' $tests 'run-physics: $(PHYSICS_TARGET)'
foreach ($entry in @(
    @{ Label = 'ECS test rule'; Pattern = '^\$\(ECS_TARGET\):' },
    @{ Label = 'physics test rule'; Pattern = '^\$\(PHYSICS_TARGET\):' }
)) {
    $rule = Get-RuleBlock $tests $entry.Pattern
    $ruleLines = $rule -split "`r?`n"
    Require-Count "$($entry.Label) archive prerequisite" $ruleLines[0] '$(BOX3D_LIB)' 1
    Require-Count "$($entry.Label) link archive" ($ruleLines[1..($ruleLines.Count - 1)] -join "`n") '$(BOX3D_LIB)' 1
}

# Shared GPU binaries already own Runtime; every such link rule must carry one
# native archive as well as the now-closed shared source graph.
$gpuRuleMatches = [regex]::Matches($tests, '(?m)^[^\s#][^\r\n]*:\s*[^\r\n]*\$\(GPU_SHARED_OBJS\)[^\r\n]*\r?\n(?:\t[^\r\n]*(?:\r?\n|$))+')
if ($gpuRuleMatches.Count -eq 0) {
    $failures.Add('no GPU_SHARED_OBJS link rules found')
}
foreach ($match in $gpuRuleMatches) {
    $archiveCount = ([regex]::Matches($match.Value, '\$\((?:BOX3D_DIR\)/libbox3d\.a|BOX3D_LIB\))')).Count
    if ($archiveCount -ne 1) {
        $firstLine = ($match.Value -split "`r?`n")[0]
        $failures.Add("GPU Runtime rule '$firstLine' expected exactly one native Box3D link archive, found $archiveCount")
    }
}

# Engine archive readiness and both application platform link graphs.
Require-Regex 'engine native Box3D library' $engine '(?m)^BOX3D_LIB\s*=\s*\$\(BOX3D_DIR\)/libbox3d\.a\s*$'
Require-Count 'engine archive Box3D prerequisite' (Get-RuleBlock $engine '^\$\(LIB\):') '$(BOX3D_LIB)' 1
foreach ($entry in @(
    @{ Label = 'viewer'; Text = $viewer },
    @{ Label = 'explorer'; Text = $explorer }
)) {
    Require-Regex "$($entry.Label) native Box3D library" $entry.Text '(?m)^BOX3D_LIB\s*=\s*\$\(BOX3D_DIR\)/libbox3d\.a\s*$'
    Require-Regex "$($entry.Label) MinGW Box3D library" $entry.Text '(?m)^WIN_BOX3D_LIB\s*=\s*\$\(BOX3D_DIR\)/build-mingw/libbox3d\.a\s*$'
    Require-Count "$($entry.Label) Linux link archive" (Get-AssignmentBlock $entry.Text 'LDLIBS') '$(BOX3D_LIB)' 1
    Require-Count "$($entry.Label) Windows link archive" (Get-AssignmentBlock $entry.Text 'WIN_LIBS') '$(WIN_BOX3D_LIB)' 1
}

if ($failures.Count -gt 0) {
    Write-Host "FAIL: Box3D Phase 2 build contract ($($failures.Count) issue(s))"
    foreach ($failure in $failures) {
        Write-Host " - $failure"
    }
    exit 1
}

Write-Host 'PASS: Box3D Phase 2 build contract'
Write-Host ' - Runtime owns one opaque context after its Flecs world member'
Write-Host ' - every Runtime source graph includes physics_context.cpp exactly once'
Write-Host ' - engine, focused tests, GPU tests, Viewer, and Explorer select one platform archive'
