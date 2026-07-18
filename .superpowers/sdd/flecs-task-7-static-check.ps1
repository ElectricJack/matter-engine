$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$failures = [System.Collections.Generic.List[string]]::new()

function Read-RepoFile([string]$relativePath) {
    return [System.IO.File]::ReadAllText((Join-Path $root $relativePath))
}

function Get-AssignmentBlock([string]$text, [string]$name) {
    $lines = $text -split "`r?`n"
    $start = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match ('^' + [regex]::Escape($name) + '\s*[:+?]?=')) {
            $start = $i
            break
        }
    }
    if ($start -lt 0) {
        return ''
    }

    $block = [System.Collections.Generic.List[string]]::new()
    for ($i = $start; $i -lt $lines.Count; ++$i) {
        $block.Add($lines[$i])
        if ($lines[$i].TrimEnd() -notmatch '\\$') {
            break
        }
    }
    return $block -join "`n"
}

function Get-ContinuedLine([string]$text, [string]$pattern) {
    $lines = $text -split "`r?`n"
    $start = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match $pattern) {
            $start = $i
            break
        }
    }
    if ($start -lt 0) {
        return ''
    }

    $block = [System.Collections.Generic.List[string]]::new()
    for ($i = $start; $i -lt $lines.Count; ++$i) {
        $block.Add($lines[$i])
        if ($lines[$i].TrimEnd() -notmatch '\\$') {
            break
        }
    }
    return $block -join "`n"
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

function Get-LiteralSources([string]$text, [string[]]$variables) {
    $sources = [System.Collections.Generic.List[string]]::new()
    foreach ($variable in $variables) {
        $block = Get-AssignmentBlock $text $variable
        foreach ($match in [regex]::Matches($block, '[^\s\\]+?\.(?:cpp|c)(?=\s|$)')) {
            $sources.Add($match.Value)
        }
    }
    return $sources
}

function Require-UniqueBasenames([string]$label, [string[]]$sources) {
    $basenames = foreach ($source in $sources) {
        [System.IO.Path]::GetFileNameWithoutExtension(($source -replace '/', '\'))
    }
    $duplicates = @($basenames | Group-Object | Where-Object Count -gt 1 | ForEach-Object Name)
    if ($duplicates.Count -gt 0) {
        $failures.Add("$label basename collision(s): $($duplicates -join ', ')")
    }
    $flecsCount = @($basenames | Where-Object { $_ -eq 'flecs' }).Count
    if ($flecsCount -ne 1) {
        $failures.Add("$label expected exactly one flecs source/object basename, found $flecsCount")
    }
}

$engine = Read-RepoFile 'MatterEngine3\Makefile'
$tests = Read-RepoFile 'MatterEngine3\tests\Makefile'
$viewer = Read-RepoFile 'MatterViewer\Makefile'

# MatterEngine3 archive contract.
Require-Regex 'engine Flecs directory' $engine '(?m)^FLECS_DIR\s*=\s*\.\./Libraries/flecs\s*$'
Require-Contains 'engine include paths' (Get-AssignmentBlock $engine 'INCLUDE_PATHS') '-I$(FLECS_DIR)'
Require-Contains 'engine C++ sources' (Get-AssignmentBlock $engine 'ME3_CPP') 'src/ecs/ecs_runtime.cpp'
Require-Contains 'engine C++ sources' (Get-AssignmentBlock $engine 'ME3_CPP') 'src/ecs/transform_system.cpp'
Require-Contains 'engine C++ objects' (Get-AssignmentBlock $engine 'ME3_OBJ') 'ecs_runtime.o'
Require-Contains 'engine C++ objects' (Get-AssignmentBlock $engine 'ME3_OBJ') 'transform_system.o'
Require-Regex 'engine Flecs C source' $engine '(?m)^FLECS_C\s*=\s*\$\(FLECS_DIR\)/flecs\.c\s*$'
Require-Regex 'engine Flecs object' $engine '(?m)^FLECS_OBJ\s*=\s*flecs\.o\s*$'
Require-Regex 'engine C++ vpath' $engine '(?m)^vpath %\.cpp .*\bsrc/ecs\b'
Require-Regex 'engine C vpath' $engine '(?m)^vpath %\.c .*\$\(FLECS_DIR\)'
Require-Regex 'engine dedicated Flecs C rule' $engine '(?ms)^\$\(FLECS_OBJ\):\s*\$\(FLECS_C\)\s*\r?\n\s*gcc -c \$< -o \$@ -std=c17 -O2 -I\$\(FLECS_DIR\)\s*$'
Require-Contains 'engine archive prerequisites' ([regex]::Match($engine, '(?m)^\$\(LIB\):.*$').Value) '$(FLECS_OBJ)'
Require-Contains 'engine archive members' ([regex]::Match($engine, '(?m)^\s*ar rcs \$\(LIB\).*$').Value) '$(FLECS_OBJ)'
Require-Contains 'engine clean' ([regex]::Match($engine, '(?m)^\s*rm -f \$\(LIB\).*$').Value) '$(FLECS_OBJ)'
Require-Count 'engine ME3_CPP excludes C Flecs source' (Get-AssignmentBlock $engine 'ME3_CPP') 'flecs.c' 0

$engineObjectText = ((Get-AssignmentBlock $engine 'QJS_OBJ'),
                     (Get-AssignmentBlock $engine 'ME3_OBJ'),
                     (Get-AssignmentBlock $engine 'MSL_OBJ'),
                     (Get-AssignmentBlock $engine 'MSL_C_OBJ'),
                     (Get-AssignmentBlock $engine 'FLECS_OBJ')) -join "`n"
$engineObjects = @([regex]::Matches($engineObjectText, '[A-Za-z0-9_]+\.o') | ForEach-Object Value)
Require-UniqueBasenames 'MatterEngine3 archive' $engineObjects

# Session-bearing tests share the one flecsc object; ECS itself still gets one.
$viewerLogicObjects = Get-AssignmentBlock $tests 'VIEWER_LOGIC_OBJS'
$gpuSharedObjects = Get-AssignmentBlock $tests 'GPU_SHARED_OBJS'
$ecsObjects = Get-AssignmentBlock $tests 'ECS_OBJS'
Require-Count 'VIEWER_LOGIC_OBJS Flecs membership' $viewerLogicObjects '$(flecsc_C_OBJS)' 1
Require-Count 'GPU_SHARED_OBJS Flecs membership' $gpuSharedObjects '$(flecsc_C_OBJS)' 1
Require-Count 'ECS_OBJS flecsc flavor membership' $ecsObjects '$(call obj_list,flecsc,$(ECS_C))' 1
Require-Count 'flecsc source union' (Get-AssignmentBlock $tests 'flecsc_C_SRCS') '$(ECS_C)' 1
Require-Regex 'flecsc uses gcc' $tests '(?m)^FLAVOR_flecsc_CC\s*:=\s*gcc\s*$'
Require-Regex 'flecsc uses C17' $tests '(?m)^FLAVOR_flecsc_FLAGS\s*:=\s*-std=c17 -O2 -I\$\(FLECS_DIR\)\s*$'

# matter_engine.cpp owns a Runtime, so its final test compile/link union must
# contain both ECS implementation TUs in addition to the single Flecs C object.
$matterEngineSourceLists = @(Get-AssignmentNamesContaining $tests '../src/matter_engine.cpp')
if ($matterEngineSourceLists.Count -ne 1 -or $matterEngineSourceLists[0] -ne 'GPU_RENDER_CPP') {
    $failures.Add("test matter_engine.cpp source owners expected only GPU_RENDER_CPP, found: $($matterEngineSourceLists -join ', ')")
}
$gpuAllCpp = Get-AssignmentBlock $tests 'GPU_ALL_CPP'
Require-Count 'GPU_ALL_CPP Runtime implementation' $gpuAllCpp '../src/ecs/ecs_runtime.cpp' 1
Require-Count 'GPU_ALL_CPP transform implementation' $gpuAllCpp '../src/ecs/transform_system.cpp' 1
Require-Count 'GPU_ALL_CPP matter-engine source closure' $gpuAllCpp '$(GPU_RENDER_CPP)' 1
Require-Count 'gpu_CPP_SRCS consumes complete GPU union' (Get-AssignmentBlock $tests 'gpu_CPP_SRCS') '$(GPU_ALL_CPP)' 1
Require-Count 'GPU_SHARED_OBJS consumes complete GPU union' $gpuSharedObjects '$(call obj_list,gpu,$(GPU_ALL_CPP))' 1

foreach ($entry in @(
    @{ Label = 'MatterViewer'; Text = $viewer; AppVars = @('APP_SRC', 'WIN_ME3_CPP', 'WIN_MSL_CPP', 'IMGUI_CORE_SRC', 'IMGUI_SRC_WIN', 'WIN_PIPELINE_C', 'QJS_C', 'FLECS_C') }
)) {
    $label = $entry.Label
    $text = $entry.Text
    Require-Regex "$label Flecs directory" $text '(?m)^FLECS_DIR\s*=\s*\.\./Libraries/flecs\s*$'
    Require-Contains "$label shared include paths" (Get-AssignmentBlock $text 'INCLUDE_PATHS') '-I$(FLECS_DIR)'
    Require-Contains "$label Windows include paths" (Get-AssignmentBlock $text 'WIN_INCLUDE_PATHS') '-I$(FLECS_DIR)'
    Require-Contains "$label engine C++ sources" (Get-AssignmentBlock $text 'WIN_ME3_CPP') '$(ME3_DIR)/src/ecs/ecs_runtime.cpp'
    Require-Contains "$label engine C++ sources" (Get-AssignmentBlock $text 'WIN_ME3_CPP') '$(ME3_DIR)/src/ecs/transform_system.cpp'
    Require-Regex "$label Flecs C source" $text '(?m)^FLECS_C\s*=\s*\$\(FLECS_DIR\)/flecs\.c\s*$'
    Require-Regex "$label Flecs source name" $text '(?m)^FLECS_NAME\s*=\s*\$\(notdir \$\(FLECS_C\)\)\s*$'
    Require-Regex "$label Flecs object" $text '(?m)^W_FLECS_OBJ\s*=\s*\$\(W_DIR\)/\$\(FLECS_NAME:\.c=\.o\)\s*$'
    Require-Contains "$label C++ vpath" (Get-ContinuedLine $text '^vpath %\.cpp ') '$(ME3_DIR)/src/ecs'
    Require-Contains "$label C vpath" ([regex]::Match($text, '(?m)^vpath %\.c .*$').Value) '$(FLECS_DIR)'
    Require-Contains "$label W_ALL_OBJ" (Get-AssignmentBlock $text 'W_ALL_OBJ') '$(W_FLECS_OBJ)'
    Require-Regex "$label dedicated Flecs C rule" $text '(?ms)^\$\(W_FLECS_OBJ\):\s*\$\(W_DIR\)/%\.o:\s*\$\(FLECS_DIR\)/%\.c \| \$\(W_DIR\)\s*\r?\n\s*\$\(WIN_CC\) -c \$< -o \$@ -std=c17 -O2 -I\$\(FLECS_DIR\).*$'
    Require-Count "$label C++ source union excludes Flecs C" (Get-AssignmentBlock $text 'WIN_ALL_CPP_SRC') '$(FLECS_C)' 0
    Require-Count "$label C++ compiler excludes Flecs rule" ([regex]::Match($text, '(?m)^.*\$\(WIN_CXX\).*$').Value) 'FLECS' 0

    $sources = @(Get-LiteralSources $text $entry.AppVars)
    Require-UniqueBasenames "$label Windows source union" $sources
}

if ($failures.Count -gt 0) {
    Write-Host "FAIL: Flecs Task 7 build contract ($($failures.Count) issue(s))"
    foreach ($failure in $failures) {
        Write-Host " - $failure"
    }
    exit 1
}

Write-Host 'PASS: Flecs Task 7 build contract'
Write-Host ' - MatterEngine3 archive has one C-compiled flecs.o plus both ECS C++ objects'
Write-Host ' - every matter_engine.cpp test flavor links both ECS C++ objects and one flecsc object'
Write-Host ' - Viewer publishes Flecs through shared includes and links one C Flecs source on Windows'
