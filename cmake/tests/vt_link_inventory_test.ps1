[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$Ninja
)

$ErrorActionPreference = 'Stop'
$commands = & $Ninja -C $BuildDirectory -t commands vt_compositor_tests
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to query the generated vt_compositor_tests command graph'
}
$linkLine = $commands | Where-Object { $_ -match 'link\.exe' } | Select-Object -Last 1
if (-not $linkLine) {
    throw 'Generated vt_compositor_tests link command was not found'
}

$expectedObjects = @(
    'vt_compositor_tests.cpp.obj',
    'vt_compositor.cpp.obj',
    'terrain_field.cpp.obj',
    'vk_context.cpp.obj',
    'vk_resources.cpp.obj',
    'streamline_bridge.cpp.obj'
)
$objectMatches = [regex]::Matches($linkLine, '[^\s"]+\.obj')
$actualObjects = @($objectMatches | ForEach-Object { [System.IO.Path]::GetFileName($_.Value) })
$actualObjects = @($actualObjects | Sort-Object -Unique)
$expectedSorted = @($expectedObjects | Sort-Object)
if (($actualObjects -join "`n") -ne ($expectedSorted -join "`n")) {
    throw "VT link object boundary changed. Expected:`n$($expectedSorted -join "`n")`nActual:`n$($actualObjects -join "`n")"
}
foreach ($forbidden in @('matter_engine_headless.lib', 'matter_autoremesher.lib',
        'matter_quickjs.lib', 'matter_profile.lib', 'matter_box3d.lib', 'matter_ozz_')) {
    if ($linkLine -match [regex]::Escape($forbidden)) {
        throw "VT link boundary contains forbidden broad dependency '$forbidden'"
    }
}
foreach ($required in @('matter_glfw.lib', 'matter_flecs.lib', 'vulkan-1.lib')) {
    if ($linkLine -notmatch [regex]::Escape($required)) {
        throw "VT link boundary is missing '$required'"
    }
}

Write-Output 'VT generated link/object inventory: PASS'
