# Shared visual-content oracle for viewer smoke screenshots. This file defines
# functions only so fixture tests can dot-source it without launching the editor.

function Assert-VulkanSceneFrame {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "scene frame is missing: $Path"
    }
    Add-Type -AssemblyName System.Drawing
    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        if ($bitmap.Width -lt 64 -or $bitmap.Height -lt 64) {
            throw "scene frame is too small to validate: $Path"
        }
        $x0 = [int]($bitmap.Width * 0.25)
        $x1 = [int]($bitmap.Width * 0.75)
        $y0 = [int]($bitmap.Height * 0.15)
        $y1 = [int]($bitmap.Height * 0.90)
        $step = [Math]::Max(2, [int]([Math]::Min($bitmap.Width, $bitmap.Height) / 150))
        $samples = 0
        $nonblack = 0
        $minimum = 255.0
        $maximum = 0.0
        $sum = 0.0
        $sumSquares = 0.0
        $edgePairs = 0
        $edges = 0
        $quantized = New-Object 'System.Collections.Generic.HashSet[int]'
        $quadrantSamples = @(0, 0, 0, 0)
        $quadrantNonblack = @(0, 0, 0, 0)
        $quadrantSum = @(0.0, 0.0, 0.0, 0.0)
        $quadrantSquares = @(0.0, 0.0, 0.0, 0.0)
        $midX = ($x0 + $x1) / 2
        $midY = ($y0 + $y1) / 2

        for ($y = $y0; $y -lt $y1; $y += $step) {
            $hasPrevious = $false
            $previousLuminance = 0.0
            for ($x = $x0; $x -lt $x1; $x += $step) {
                $pixel = $bitmap.GetPixel($x, $y)
                $luminance = 0.2126 * $pixel.R + 0.7152 * $pixel.G + 0.0722 * $pixel.B
                ++$samples
                if ($luminance -gt 15.0) { ++$nonblack }
                $minimum = [Math]::Min($minimum, $luminance)
                $maximum = [Math]::Max($maximum, $luminance)
                $sum += $luminance
                $sumSquares += $luminance * $luminance
                [void]$quantized.Add(
                    (([int]($pixel.R / 32)) -shl 6) -bor
                    (([int]($pixel.G / 32)) -shl 3) -bor
                    ([int]($pixel.B / 32)))
                if ($hasPrevious) {
                    ++$edgePairs
                    if ([Math]::Abs($luminance - $previousLuminance) -gt 10.0) {
                        ++$edges
                    }
                }
                $previousLuminance = $luminance
                $hasPrevious = $true

                $quadrant = 0
                if ($x -ge $midX) { $quadrant += 1 }
                if ($y -ge $midY) { $quadrant += 2 }
                ++$quadrantSamples[$quadrant]
                if ($luminance -gt 15.0) { ++$quadrantNonblack[$quadrant] }
                $quadrantSum[$quadrant] += $luminance
                $quadrantSquares[$quadrant] += $luminance * $luminance
            }
        }

        if ($samples -eq 0) { throw "scene frame has no sampled world region: $Path" }
        $coverage = $nonblack / [double]$samples
        $mean = $sum / $samples
        $variance = [Math]::Max(0.0, $sumSquares / $samples - $mean * $mean)
        $standardDeviation = [Math]::Sqrt($variance)
        $edgeCoverage = if ($edgePairs) { $edges / [double]$edgePairs } else { 0.0 }
        if ($coverage -lt 0.15) {
            throw "scene frame has insufficient nonblack world coverage ($([Math]::Round(100*$coverage, 2))%): $Path"
        }
        if (($maximum - $minimum) -lt 45.0 -or $standardDeviation -lt 12.0) {
            throw "scene frame lacks dynamic range/variance (range=$([Math]::Round($maximum-$minimum, 2)), sigma=$([Math]::Round($standardDeviation, 2))): $Path"
        }
        if ($quantized.Count -lt 12) {
            throw "scene frame has too few quantized colors ($($quantized.Count)): $Path"
        }
        if ($edgeCoverage -lt 0.005) {
            throw "scene frame lacks retained silhouettes/edges ($([Math]::Round(100*$edgeCoverage, 3))%): $Path"
        }
        $variedQuadrants = 0
        for ($quadrant = 0; $quadrant -lt 4; ++$quadrant) {
            $count = $quadrantSamples[$quadrant]
            if ($count -eq 0 -or $quadrantNonblack[$quadrant] / [double]$count -lt 0.05) {
                throw "scene frame quadrant $quadrant has insufficient foreground coverage: $Path"
            }
            $quadrantMean = $quadrantSum[$quadrant] / $count
            $quadrantVariance = [Math]::Max(
                0.0, $quadrantSquares[$quadrant] / $count - $quadrantMean * $quadrantMean)
            if ([Math]::Sqrt($quadrantVariance) -ge 8.0) { ++$variedQuadrants }
        }
        if ($variedQuadrants -lt 3) {
            throw "scene frame lacks spatially distributed detail ($variedQuadrants/4 varied quadrants): $Path"
        }
        Write-Output (("scene oracle: coverage={0:F2}% range={1:F1} sigma={2:F1} " +
                       "colors={3} edges={4:F2}% quadrants={5}/4") -f
                      (100.0 * $coverage), ($maximum - $minimum),
                      $standardDeviation, $quantized.Count,
                      (100.0 * $edgeCoverage), $variedQuadrants)
    } finally {
        $bitmap.Dispose()
    }
}
