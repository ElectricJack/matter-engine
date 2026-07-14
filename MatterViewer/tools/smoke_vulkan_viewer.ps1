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
        $colors = [System.Collections.Generic.HashSet[int]]::new()
        $stepX = [Math]::Max(1, [int]($bitmap.Width / 64))
        $stepY = [Math]::Max(1, [int]($bitmap.Height / 64))
        for ($y = 0; $y -lt $bitmap.Height; $y += $stepY) {
            for ($x = 0; $x -lt $bitmap.Width; $x += $stepX) {
                [void]$colors.Add($bitmap.GetPixel($x, $y).ToArgb())
            }
        }
        if ($colors.Count -lt 8) {
            throw "screenshot has only $($colors.Count) sampled colors: $Path"
        }
    } finally {
        $bitmap.Dispose()
    }
}

function Invoke-ViewerCase([string]$Name, [bool]$Resize,
                           [int]$Width, [int]$Height) {
    $png = Join-Path $OutputDir "$Name.png"
    Remove-Item -Force $png -ErrorAction SilentlyContinue
    $env:MATTER_WORLD = 'CornellBox'
    $env:MATTER_SCREENSHOT = $png
    if ($Resize) { $env:MATTER_TEST_RESIZE = '1' }
    else { Remove-Item Env:MATTER_TEST_RESIZE -ErrorAction SilentlyContinue }
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
    Assert-Png $png $Width $Height
    Write-Output "$Name PASS: $Width x $Height, validation errors 0"
}

$saved = @{}
foreach ($name in @('MATTER_WORLD','MATTER_SCREENSHOT','MATTER_TEST_RESIZE',
                    'VK_LOADER_LAYERS_DISABLE')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    if (-not $env:VK_LOADER_LAYERS_DISABLE) {
        $env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
    }
    Invoke-ViewerCase 'cornell' $false 1280 720
    Invoke-ViewerCase 'cornell-resize' $true 960 540
    Write-Output 'vulkan-viewer runtime smoke: PASS'
} finally {
    foreach ($name in $saved.Keys) {
        [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
    }
}
