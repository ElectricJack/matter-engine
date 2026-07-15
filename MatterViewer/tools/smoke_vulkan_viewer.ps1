param(
    [string]$ViewerPath,
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $ViewerPath) { $ViewerPath = Join-Path $root 'MatterViewer\viewer.exe' }
if (-not $OutputDir) { $OutputDir = Join-Path $root '.codex-tmp\vulkan-viewer-smoke' }
$ViewerPath = (Resolve-Path $ViewerPath).Path
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
Add-Type -AssemblyName System.Drawing

function Assert-Png([string]$Path, [int]$ExpectedWidth, [int]$ExpectedHeight) {
    if (-not (Test-Path $Path)) { throw "screenshot was not written: $Path" }
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    [byte[]]$signature = 137,80,78,71,13,10,26,10
    if ($bytes.Length -lt 8 -or -not [System.Linq.Enumerable]::SequenceEqual(
            [byte[]]$bytes[0..7], $signature)) {
        throw "PNG signature mismatch: $Path"
    }
    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        if ($bitmap.Width -ne $ExpectedWidth -or $bitmap.Height -ne $ExpectedHeight) {
            throw "unexpected PNG dimensions $($bitmap.Width)x$($bitmap.Height): $Path"
        }
        # Scene diversity is asserted below by independent nonblack, red,
        # green, and gray coverage.  Do not count UI colors as scene content.
        $x0 = [int]($bitmap.Width * 0.25); $x1 = [int]($bitmap.Width * 0.75)
        $y0 = [int]($bitmap.Height * 0.15); $y1 = [int]($bitmap.Height * 0.90)
        $world = 0; $nonblack = 0; $red = 0; $green = 0; $gray = 0
        for ($y = $y0; $y -lt $y1; $y += 2) {
            for ($x = $x0; $x -lt $x1; $x += 2) {
                $p = $bitmap.GetPixel($x, $y); ++$world
                if (($p.R + $p.G + $p.B) -gt 45) { ++$nonblack }
                if ($p.R -gt ($p.G * 1.35) -and $p.R -gt ($p.B * 1.35) -and $p.R -gt 35) { ++$red }
                if ($p.G -gt ($p.R * 1.25) -and $p.G -gt ($p.B * 1.25) -and $p.G -gt 30) { ++$green }
                if ([Math]::Abs($p.R-$p.G) -lt 18 -and
                    [Math]::Abs($p.G-$p.B) -lt 18 -and $p.R -gt 35) { ++$gray }
            }
        }
        if ($nonblack -lt ($world * 0.20)) { throw "insufficient nonblack world coverage: $Path" }
        if ($red -lt ($world * 0.015)) { throw "red Cornell region missing: $Path" }
        if ($green -lt ($world * 0.015)) { throw "green Cornell region missing: $Path" }
        if ($gray -lt ($world * 0.04)) { throw "gray Cornell region missing: $Path" }
    } finally {
        $bitmap.Dispose()
    }
    $hash = (Get-FileHash -Algorithm SHA256 $Path).Hash
    if (-not $hash -or $hash -match '^0+$') { throw "invalid PNG hash: $Path" }
    Write-Output "PNG SHA256 $hash"
}

function Assert-UiOverlay([string]$Path, [bool]$ExpectedVisible) {
    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        # ImGui's blue-gray selected rows are a stable marker in the upper-left
        # panels and do not overlap the Cornell geometry in a hidden-UI frame.
        $markers = 0
        for ($y = 15; $y -lt [Math]::Min(310, $bitmap.Height); $y += 2) {
            for ($x = 15; $x -lt [Math]::Min(230, $bitmap.Width); $x += 2) {
                $p = $bitmap.GetPixel($x, $y)
                if ($p.R -ge 50 -and $p.R -le 105 -and
                    $p.G -ge 70 -and $p.G -le 130 -and
                    $p.B -ge 90 -and $p.B -le 155 -and
                    ($p.B - $p.R) -ge 25 -and ($p.B - $p.G) -ge 12) {
                    ++$markers
                }
            }
        }
        if ($ExpectedVisible -and $markers -lt 100) {
            throw "expected debug UI overlay is absent: $Path"
        }
        if (-not $ExpectedVisible -and $markers -ge 100) {
            throw "debug UI overlay obscures scene verification: $Path"
        }
    } finally {
        $bitmap.Dispose()
    }
}

function Assert-PeImports([string]$Path) {
    $objdump = @('C:\msys64\ucrt64\bin\objdump.exe',
                 'C:\msys64\usr\bin\objdump.exe') |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $objdump) { throw 'objdump unavailable for PE import inspection' }
    $imports = (& $objdump -p $Path 2>&1 | Out-String)
    foreach ($forbidden in @('opengl32','ChoosePixelFormat','SetPixelFormat',
                              'SwapBuffers','cuGraphicsGL','nvcuda','cuDevice')) {
        if ($imports -match [regex]::Escape($forbidden)) {
            throw "forbidden PE import/symbol ${forbidden}: $Path"
        }
    }
}

function Invoke-ViewerCase([string]$Name, [bool]$Resize,
                           [int]$Width, [int]$Height, [bool]$TextureOverride,
                           [bool]$HideUi, [bool]$AssertMaterials,
                           [bool]$DisableRt = $false) {
    $png = Join-Path $OutputDir "$Name.png"
    Remove-Item -Force $png -ErrorAction SilentlyContinue
    $env:MATTER_WORLD = 'CornellBox'
    $env:MATTER_CACHE_ROOT = Join-Path $OutputDir "cache-$Name"
    $env:MATTER_VK_DIAGNOSTIC_MATERIALS = '1'
    Remove-Item -Recurse -Force $env:MATTER_CACHE_ROOT -ErrorAction SilentlyContinue
    $env:MATTER_SCREENSHOT = $png
    if ($Resize) { $env:MATTER_TEST_RESIZE = '1' }
    else { Remove-Item Env:MATTER_TEST_RESIZE -ErrorAction SilentlyContinue }
    if ($HideUi) { $env:MATTER_HIDE_UI = '1' }
    else { Remove-Item Env:MATTER_HIDE_UI -ErrorAction SilentlyContinue }
    if ($DisableRt) { $env:MATTER_DISABLE_VK_RT = '1' }
    else { Remove-Item Env:MATTER_DISABLE_VK_RT -ErrorAction SilentlyContinue }
    if ($TextureOverride) {
        $env:MATTER_VK_DIAGNOSTIC_GROUND_TILESET_MATERIAL = '8'
        $env:MATTER_VK_DIAGNOSTIC_GROUND_TILESET_PRIOR_SLOT = '2'
    } else {
        Remove-Item Env:MATTER_VK_DIAGNOSTIC_GROUND_TILESET_MATERIAL `
            -ErrorAction SilentlyContinue
        Remove-Item Env:MATTER_VK_DIAGNOSTIC_GROUND_TILESET_PRIOR_SLOT `
            -ErrorAction SilentlyContinue
    }
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $log = @(& $ViewerPath 2>&1 | ForEach-Object { "$_" })
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorAction
    }
    $log | ForEach-Object { Write-Output $_ }
    if ($exitCode -ne 0) { throw "$Name viewer exited with code $exitCode" }
    $joined = $log -join "`n"
    if ($joined -match 'FATAL: Vulkan validation errors') {
        throw "$Name reported Vulkan validation errors"
    }
    if ($joined -notmatch 'screenshot written to') {
        throw "$Name did not confirm screenshot completion"
    }
    if ($joined -notmatch 'selected world CornellBox hash ([0-9a-fA-F]{16})') {
        throw "$Name did not report selected world CornellBox hash"
    }
    $escapedExtent = "${Width}x${Height}"
    if ($joined -notmatch
            "DLSS selected=Native active=Native internal=$escapedExtent output=$escapedExtent resets=[0-9]+ reason=(?!none)(.+)") {
        throw "$Name did not truthfully report Native DLSS fallback, extents, and reason"
    }
    $expectedRtEnabled = if ($DisableRt) { 'false' } else { 'true' }
    if ($joined -notmatch "Vulkan RT available=true enabled=$expectedRtEnabled reason=.+") {
        throw "$Name did not report expected Vulkan RT enabled state $expectedRtEnabled"
    }
    if ($joined -notmatch
            'Vulkan material diagnostic:.*ids=[1-9][0-9]*.*tinted=[1-9][0-9]*.*red=[1-9][0-9]*.*green=[1-9][0-9]*') {
        throw "$Name did not preserve Cornell material IDs and red/green tints through RasterMeshData"
    }
    if ($TextureOverride) {
        $warning = 'Vulkan milestone: ground material texture sampling is not available'
        if (-not $joined.Contains($warning)) {
            throw "$Name did not exercise the rendered packed-material warning"
        }
        if (-not $joined.Contains(
                'Vulkan diagnostic: seeded ground tileset material 8 prior packed slot 2')) {
            throw "$Name did not seed a non-default packed prior material slot"
        }
        if (-not $joined.Contains(
                'Vulkan diagnostic: restored ground tileset material 8 to packed slot 2')) {
            throw "$Name did not restore the exact packed prior material slot"
        }
    }
    if ($AssertMaterials) { Assert-Png $png $Width $Height }
    else {
        if (-not (Test-Path $png)) { throw "screenshot was not written: $png" }
        $bitmap = [System.Drawing.Bitmap]::FromFile($png)
        try {
            if ($bitmap.Width -ne $Width -or $bitmap.Height -ne $Height) {
                throw "unexpected PNG dimensions $($bitmap.Width)x$($bitmap.Height): $png"
            }
        } finally {
            $bitmap.Dispose()
        }
    }
    Assert-UiOverlay $png (-not $HideUi)
    Write-Output "$Name PASS: $Width x $Height, validation errors 0"
}

$saved = @{}
foreach ($name in @('MATTER_WORLD','MATTER_SCREENSHOT','MATTER_TEST_RESIZE',
                    'MATTER_HIDE_UI',
                    'MATTER_CACHE_ROOT',
                    'MATTER_VK_DIAGNOSTIC_MATERIALS',
                    'MATTER_VK_DIAGNOSTIC_GROUND_TILESET_MATERIAL',
                    'MATTER_VK_DIAGNOSTIC_GROUND_TILESET_PRIOR_SLOT',
                    'MATTER_DISABLE_VK_RT',
                    'VK_LAYER_PATH','PATH')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $layerCandidates = @()
    if ($env:VULKAN_SDK) { $layerCandidates += (Join-Path $env:VULKAN_SDK 'Bin') }
    $layerCandidates += 'C:\msys64\ucrt64\bin'
    $layerCandidates += 'C:\msys64\ucrt64\share\vulkan\explicit_layer.d'
    $layerDir = $layerCandidates | Where-Object {
        Test-Path (Join-Path $_ 'VkLayer_khronos_validation.json')
    } | Select-Object -First 1
    if (-not $layerDir) {
        throw 'Vulkan validation layer unavailable; install MSYS2 vulkan-validation-layers'
    }
    $env:VK_LAYER_PATH = $layerDir
    $msysBin = 'C:\msys64\ucrt64\bin'
    if (Test-Path $msysBin) { $env:PATH = "$msysBin;$env:PATH" }
    Assert-PeImports $ViewerPath
    $features = Join-Path (Split-Path $ViewerPath) 'build\windows\build_features.txt'
    if (-not (Test-Path $features)) { $features = Join-Path $root 'MatterViewer\build\windows\build_features.txt' }
    $manifest = Get-Content -Raw $features
    foreach ($feature in @('VULKAN=1','CUDA_AVAILABLE=1','OPTIX_AVAILABLE=1',
                            'CUDA_ACTIVE=1','OPTIX_ACTIVE=0','OPENGL=0')) {
        if (-not $manifest.Contains($feature)) { throw "feature manifest missing $feature" }
    }
    Invoke-ViewerCase 'cornell-demo' $false 1280 720 $false $false $false
    Invoke-ViewerCase 'cornell-materials' $false 1280 720 $false $true $true
    Invoke-ViewerCase 'cornell-resize' $true 960 540 $false $true $true
    Invoke-ViewerCase 'cornell-override' $false 1280 720 $true $true $true
    Invoke-ViewerCase 'cornell-rt-disabled' $false 1280 720 $false $true $true $true
    Write-Output 'vulkan-viewer runtime smoke: PASS'
} finally {
    foreach ($name in $saved.Keys) {
        [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
    }
}
