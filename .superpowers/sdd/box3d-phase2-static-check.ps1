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

function Get-AllAssignmentBlocks([string]$text, [string]$name) {
    $lines = $text -split "`r?`n"
    $blocks = [System.Collections.Generic.List[string]]::new()
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -notmatch ('^\s*' + [regex]::Escape($name) + '\s*[:+?]?=')) {
            continue
        }
        $block = [System.Collections.Generic.List[string]]::new()
        for ($j = $i; $j -lt $lines.Count; ++$j) {
            $block.Add($lines[$j])
            if ($lines[$j].TrimEnd() -notmatch '\\$') {
                $i = $j
                break
            }
        }
        $blocks.Add($block -join "`n")
    }
    return $blocks
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

function Get-AssignmentNamesContaining([string]$text, [string]$needle) {
    $names = [System.Collections.Generic.List[string]]::new()
    foreach ($match in [regex]::Matches($text, '(?m)^([A-Za-z0-9_]+)\s*[:+?]?=')) {
        $name = $match.Groups[1].Value
        if ((Get-AssignmentBlock $text $name).Contains($needle)) {
            $names.Add($name)
        }
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

function Require-UniqueBasenames([string]$label, [string[]]$sources) {
    $basenames = foreach ($source in $sources) {
        [System.IO.Path]::GetFileNameWithoutExtension(($source -replace '/', '\'))
    }
    $duplicates = @($basenames | Group-Object | Where-Object Count -gt 1 |
        ForEach-Object Name)
    if ($duplicates.Count -gt 0) {
        $failures.Add("$label basename collision(s): $($duplicates -join ', ')")
    }
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
$buildAll = Read-RepoFile 'build-all.sh'
$runtimeHeader = Read-RepoFile 'MatterEngine3\src\ecs\ecs_runtime.h'
$runtimeSource = Read-RepoFile 'MatterEngine3\src\ecs\ecs_runtime.cpp'
$contextHeader = Read-RepoFile 'MatterEngine3\src\ecs\physics_context.h'
$contextSource = Read-RepoFile 'MatterEngine3\src\ecs\physics_context.cpp'
$shapesSource = Read-RepoFile 'MatterEngine3\src\ecs\physics_shapes.cpp'
$systemsSource = Read-RepoFile 'MatterEngine3\src\ecs\physics_systems.cpp'
$publicPhysics = Read-RepoFile 'MatterEngine3\include\matter\physics.h'

# Box3D stays private and Runtime owns its context after the Flecs world member.
# Every published engine header remains Box3D-opaque, not just physics.h.
$publicHeaders = Get-ChildItem -LiteralPath (Join-Path $root 'MatterEngine3\include') `
    -Recurse -File -Include *.h,*.hpp
foreach ($header in $publicHeaders) {
    $headerText = [System.IO.File]::ReadAllText($header.FullName)
    if ($headerText -match '(?i)(?:#\s*include\s*[<"]box3d/|\bb3[A-Z][A-Za-z0-9_]*\b)') {
        $relative = $header.FullName.Substring($root.Length + 1)
        $failures.Add("public header '$relative' leaks Box3D")
    }
}
Require-Count 'public physics header Box3D includes' $publicPhysics 'box3d/' 0
Require-Regex 'private context implementation Box3D include' $contextSource '#include\s+[<"]box3d/box3d\.h[>"]'
Require-Count 'private context header opaque Box3D storage' $contextHeader 'b3WorldId' 0
Require-Contains 'Runtime out-of-line destructor declaration' $runtimeHeader '~Runtime();'
Require-Regex 'Runtime member destruction order' $runtimeHeader 'flecs::world world_;\s*std::unique_ptr<physics::detail::PhysicsContext> physics_;'
Require-Regex 'Runtime imports PhysicsModule' $runtimeSource 'world_\.import<physics::PhysicsModule>\(\);'
Require-Regex 'Runtime publishes private context ref' $runtimeSource 'world_\.set<physics::detail::PhysicsContextRef>'

# Discover rather than hard-code every literal assignment that owns Runtime.
foreach ($entry in @(
    @{ Label = 'engine'; Text = $engine; Runtime = 'src/ecs/ecs_runtime.cpp'; Prefix = 'src/ecs/' },
    @{ Label = 'tests'; Text = $tests; Runtime = '../src/ecs/ecs_runtime.cpp'; Prefix = '../src/ecs/' },
    @{ Label = 'viewer'; Text = $viewer; Runtime = '$(ME3_DIR)/src/ecs/ecs_runtime.cpp'; Prefix = '$(ME3_DIR)/src/ecs/' }
)) {
    $owners = @(Get-AssignmentNamesContaining $entry.Text $entry.Runtime)
    if ($owners.Count -eq 0) {
        $failures.Add("$($entry.Label) has no discovered Runtime-bearing assignment")
    }
    foreach ($owner in $owners) {
        $block = Get-AssignmentBlock $entry.Text $owner
        Require-Count "$($entry.Label) $owner Runtime" $block $entry.Runtime 1
        Require-Count "$($entry.Label) $owner PhysicsContext" $block ($entry.Prefix + 'physics_context.cpp') 1
        Require-Count "$($entry.Label) $owner PhysicsShapes" $block ($entry.Prefix + 'physics_shapes.cpp') 1
        Require-Count "$($entry.Label) $owner PhysicsSystems" $block ($entry.Prefix + 'physics_systems.cpp') 1
    }
}

$engineObjects = Get-AssignmentBlock $engine 'ME3_OBJ'
Require-Count 'engine ME3_OBJ PhysicsContext' $engineObjects 'physics_context.o' 1
Require-Count 'engine ME3_OBJ PhysicsShapes' $engineObjects 'physics_shapes.o' 1
Require-Count 'engine ME3_OBJ PhysicsSystems' $engineObjects 'physics_systems.o' 1
Require-Regex 'engine bridge C++17 flags' $engine '(?m)^CFLAGS\s*=\s*-std=c\+\+17\b'
Require-Regex 'tests bridge C++17 flags' $tests '(?m)^CFLAGS\s*=\s*-std=c\+\+17\b'
Require-Count 'tests GPU shared graph consumes complete runtime union' `
    (Get-AssignmentBlock $tests 'GPU_SHARED_OBJS') '$(call obj_list,gpu,$(GPU_ALL_CPP))' 1
Require-Count 'tests ECS object graph consumes ECS source union' `
    (Get-AssignmentBlock $tests 'ECS_OBJS') '$(call obj_list,def,$(ECS_CPP))' 1
Require-Count 'tests physics object graph consumes physics source union' `
    (Get-AssignmentBlock $tests 'PHYSICS_OBJS') '$(call obj_list,def,$(PHYSICS_CPP))' 1

# Shared compiler include graphs must all see the pinned Box3D headers.
foreach ($entry in @(
    @{ Label = 'engine'; Text = $engine; Public = '-Iinclude' },
    @{ Label = 'tests'; Text = $tests; Public = '-I../include' },
    @{ Label = 'viewer'; Text = $viewer; Public = '-I$(ME3_DIR)/include' }
)) {
    $includeBlock = Get-AssignmentBlock $entry.Text 'INCLUDE_PATHS'
    Require-Count "$($entry.Label) shared Box3D include" $includeBlock '-I$(BOX3D_DIR)/include' 1
    Require-Count "$($entry.Label) published MatterEngine3 include" $includeBlock $entry.Public 1
}

# Flecs and Box3D are C17; all engine bridge sources stay in C++17 source unions.
Require-Regex 'engine Flecs C17 rule' $engine '(?ms)^\$\(FLECS_OBJ\):.*?\n\s*gcc -c \$< -o \$@ -std=c17\b'
Require-Regex 'tests shared Flecs C17 flavor' $tests '(?m)^FLAVOR_flecsc_FLAGS\s*:=\s*-std=c17\b'
Require-Regex 'tests focused Flecs C17 flavor' $tests '(?m)^FLAVOR_physicsc_FLAGS\s*:=\s*-std=c17\b'
Require-Regex 'viewer Windows Flecs C17 rule' $viewer '(?ms)^\$\(W_FLECS_OBJ\):.*?\n\s*\$\(WIN_CC\).*?-std=c17\b'
Require-Regex 'viewer Box3D C17 build' $viewer '\$\(MINGW_CC\) -std=c17\b'

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
    $objectUnion = if ($entry.Label -eq 'ECS test rule') { '$(ECS_OBJS)' } else { '$(PHYSICS_OBJS)' }
    Require-Count "$($entry.Label) object-union prerequisite" $ruleLines[0] $objectUnion 1
    Require-Count "$($entry.Label) link object union" `
        ($ruleLines[1..($ruleLines.Count - 1)] -join "`n") $objectUnion 1
    Require-Count "$($entry.Label) archive prerequisite" $ruleLines[0] '$(BOX3D_LIB)' 1
    Require-Count "$($entry.Label) link archive" ($ruleLines[1..($ruleLines.Count - 1)] -join "`n") '$(BOX3D_LIB)' 1
}

# GPU rules expand LDLIBS and add one archive directly. Keep every platform
# LDLIBS assignment Box3D-free so their effective link contains one archive.
foreach ($linkBlock in @(Get-AllAssignmentBlocks $tests 'LDLIBS')) {
    if ($linkBlock -match '(?:BOX3D_LIB|BOX3D_DIR)') {
        $failures.Add('tests LDLIBS assignments must not contain a Box3D archive')
    }
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
    @{ Label = 'viewer'; Text = $viewer }
)) {
    Require-Regex "$($entry.Label) native Box3D library" $entry.Text '(?m)^BOX3D_LIB\s*=\s*\$\(BOX3D_DIR\)/libbox3d\.a\s*$'
    Require-Regex "$($entry.Label) MinGW Box3D library" $entry.Text '(?m)^WIN_BOX3D_LIB\s*=\s*\$\(BOX3D_DIR\)/build-mingw/libbox3d\.a\s*$'
    Require-Count "$($entry.Label) Linux link archive" (Get-AssignmentBlock $entry.Text 'LDLIBS') '$(BOX3D_LIB)' 1
    Require-Count "$($entry.Label) Windows link archive" (Get-AssignmentBlock $entry.Text 'WIN_LIBS') '$(WIN_BOX3D_LIB)' 1
    Require-Regex "$($entry.Label) bridge C++17 flags" $entry.Text '(?m)^CFLAGS\s*=\s*-std=c\+\+17\b'
    Require-Count "$($entry.Label) Windows include publishes shared paths" `
        (Get-AssignmentBlock $entry.Text 'WIN_INCLUDE_PATHS') '$(INCLUDE_PATHS)' 1
    Require-Count "$($entry.Label) flattened C++ union consumes engine sources" `
        (Get-AssignmentBlock $entry.Text 'WIN_ALL_CPP_SRC') '$(WIN_ME3_CPP)' 1
    Require-Count "$($entry.Label) final Windows object union consumes C++ objects" `
        (Get-AssignmentBlock $entry.Text 'W_ALL_OBJ') '$(W_CPP_OBJ)' 1

    $linuxRule = Get-RuleBlock $entry.Text '^\$\((?:TARGET|APP_TARGET)\):|^viewer:'
    Require-Count "$($entry.Label) final Linux link consumes LDLIBS" $linuxRule '$(LDLIBS)' 1
    Require-Count "$($entry.Label) final Linux link has no direct native archive" $linuxRule '$(BOX3D_LIB)' 0
    Require-Count "$($entry.Label) final Linux link has no direct literal archive" $linuxRule '$(BOX3D_DIR)/libbox3d.a' 0
    $windowsRule = Get-RuleBlock $entry.Text '^windows:'
    Require-Count "$($entry.Label) final Windows link consumes WIN_LIBS" $windowsRule '$(WIN_LIBS)' 1
    Require-Count "$($entry.Label) final Windows link has no direct MinGW archive" $windowsRule '$(WIN_BOX3D_LIB)' 0
    Require-Count "$($entry.Label) final Windows link has no direct literal archive" $windowsRule '$(BOX3D_DIR)/build-mingw/libbox3d.a' 0
}


# Physics is an independent standard gate and runs once before legacy suites.
$engineTestRule = Get-RuleBlock $engine '^test:'
Require-Count 'engine standard test delegates ECS once' $engineTestRule 'run-ecs' 1
Require-Count 'engine standard test delegates physics once' $engineTestRule 'run-physics' 1
if ($engineTestRule.IndexOf('run-physics') -lt $engineTestRule.IndexOf('run-ecs')) {
    $failures.Add('engine standard test must run ECS before physics')
}
$engineSuiteLoops = @([regex]::Matches(
    $buildAll, '(?m)^\s*for tgt in ([^;\r\n]+); do\s*$') |
    Where-Object { $_.Groups[1].Value -match '(?:^|\s)run-partv2(?:\s|$)' })
if ($engineSuiteLoops.Count -ne 1) {
    $failures.Add("build-all expected one MatterEngine3 suite loop, found $($engineSuiteLoops.Count)")
} else {
    $suiteTokens = @($engineSuiteLoops[0].Groups[1].Value -split '\s+' |
        Where-Object { $_ -ne '' })
    foreach ($token in @('run-ecs', 'run-physics', 'run-partv2')) {
        $count = @($suiteTokens | Where-Object { $_ -eq $token }).Count
        if ($count -ne 1) {
            $failures.Add("build-all MatterEngine3 suite expected one '$token', found $count")
        }
    }
    $ecsIndex = [Array]::IndexOf($suiteTokens, 'run-ecs')
    $physicsIndex = [Array]::IndexOf($suiteTokens, 'run-physics')
    $legacyIndex = [Array]::IndexOf($suiteTokens, 'run-partv2')
    if (-not (0 -le $ecsIndex -and $ecsIndex -lt $physicsIndex -and
              $physicsIndex -lt $legacyIndex)) {
        $failures.Add('build-all MatterEngine3 suite must order run-ecs, run-physics, then legacy targets')
    }
}

# Windows object rules flatten paths, so every source basename must be unique.
Require-UniqueBasenames 'MatterViewer Windows source union' @(
    Get-LiteralSources $viewer @('APP_SRC', 'WIN_ME3_CPP', 'WIN_MSL_CPP',
        'IMGUI_CORE_SRC', 'IMGUI_SRC_WIN', 'WIN_PIPELINE_C', 'QJS_C', 'FLECS_C'))

if ($failures.Count -gt 0) {
    Write-Host "FAIL: Box3D Phase 2 build contract ($($failures.Count) issue(s))"
    foreach ($failure in $failures) {
        Write-Host " - $failure"
    }
    exit 1
}

Write-Host 'PASS: Box3D Phase 2 build contract'
Write-Host ' - Runtime owns one opaque context after its Flecs world member'
Write-Host ' - every Runtime source graph includes context, shapes, and systems exactly once'
Write-Host ' - final test/application recipes consume exactly one selected Box3D archive'
Write-Host ' - every public header is Box3D-opaque and Windows flattened basenames are unique'
Write-Host ' - C17 C dependencies and independent standard physics gates are closed'
