[CmdletBinding()]
param([string]$RepositoryRoot)

$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
}
. (Join-Path $RepositoryRoot 'MatterEditor\tools\vulkan_scene_oracle.ps1')
Add-Type -AssemblyName System.Drawing

$scratch = Join-Path ([IO.Path]::GetTempPath()) ("matter-scene-oracle-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

function Write-Solid([string]$Path, [System.Drawing.Color]$Color) {
    $bitmap = New-Object System.Drawing.Bitmap 320, 180
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear($Color)
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Assert-Rejected([string]$Name, [string]$Path) {
    $rejected = $false
    try { Assert-VulkanSceneFrame -Path $Path }
    catch { $rejected = $true; Write-Output "scene oracle fixture: $Name rejected ($($_.Exception.Message))" }
    if (-not $rejected) { throw "scene oracle accepted invalid $Name frame" }
}

try {
    $black = Join-Path $scratch 'black.png'
    $uniform = Join-Path $scratch 'uniform.png'
    $control = Join-Path $scratch 'control.png'
    Write-Solid $black ([System.Drawing.Color]::Black)
    Write-Solid $uniform ([System.Drawing.Color]::FromArgb(255, 76, 91, 112))

    $bitmap = New-Object System.Drawing.Bitmap 320, 180
    try {
        for ($y = 0; $y -lt $bitmap.Height; ++$y) {
            for ($x = 0; $x -lt $bitmap.Width; ++$x) {
                $band = [int](($x / 16) % 12)
                $r = [Math]::Min(255, 18 + $band * 17 + [int]($y / 5))
                $g = [Math]::Min(255, 12 + [int]($x / 3))
                $b = [Math]::Min(255, 20 + [int](($bitmap.Height - $y) / 2))
                $bitmap.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, $r, $g, $b))
            }
        }
        $bitmap.Save($control, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $bitmap.Dispose()
    }

    Assert-Rejected 'black' $black
    Assert-Rejected 'uniform/corrupt' $uniform
    Assert-VulkanSceneFrame -Path $control
    Write-Output 'Vulkan scene-frame oracle fixtures: PASS (3/3)'
} finally {
    if (Test-Path -LiteralPath $scratch) {
        Remove-Item -LiteralPath $scratch -Recurse -Force
    }
}
