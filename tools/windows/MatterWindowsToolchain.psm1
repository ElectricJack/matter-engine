Set-StrictMode -Version Latest

function Require-MatterFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description was not found: $Path"
    }

    return $Path
}

function Resolve-MatterWindowsToolchain {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [switch]$Json
    )

    if ($RepositoryRoot.StartsWith('\\')) {
        throw "Repository root must be on a local Windows drive, not a UNC path: $RepositoryRoot"
    }

    if (-not (Test-Path -LiteralPath $RepositoryRoot -PathType Container)) {
        throw "Repository root was not found: $RepositoryRoot"
    }

    $repositoryRootPath = (Resolve-Path -LiteralPath $RepositoryRoot).Path
    if ($repositoryRootPath.StartsWith('\\')) {
        throw "Repository root must be on a local Windows drive, not a UNC path: $repositoryRootPath"
    }

    $vswhere = Require-MatterFile -Path (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe') -Description 'Visual Studio Locator (vswhere.exe)'
    $visualStudioRoot = @(
        & $vswhere -version '[17.0,18.0)' -products '*' `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
    ) | Where-Object { $_ } | Select-Object -First 1
    if (-not $visualStudioRoot) {
        throw 'Visual Studio 2022 with Microsoft.VisualStudio.Component.VC.Tools.x86.x64 was not found'
    }
    $visualStudioRoot = (Resolve-Path -LiteralPath $visualStudioRoot).Path

    $msvcToolsVersion = '14.44.35207'
    $windowsSdkVersion = '10.0.26100.0'
    $vsDevCmd = Require-MatterFile -Path (Join-Path $visualStudioRoot 'Common7\Tools\VsDevCmd.bat') -Description 'Visual Studio developer command prompt'
    Require-MatterFile -Path (Join-Path $visualStudioRoot "VC\Tools\MSVC\$msvcToolsVersion\bin\Hostx64\x64\cl.exe") -Description "MSVC v143 x64 compiler $msvcToolsVersion" | Out-Null
    $cmake = Require-MatterFile -Path (Join-Path $visualStudioRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe') -Description 'Visual Studio-bundled CMake'
    $ninja = Require-MatterFile -Path (Join-Path $visualStudioRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe') -Description 'Visual Studio-bundled Ninja'

    $windowsKitsRoot = 'C:\Program Files (x86)\Windows Kits\10'
    Require-MatterFile -Path (Join-Path $windowsKitsRoot "Include\$windowsSdkVersion\um\Windows.h") -Description "Windows SDK $windowsSdkVersion UM headers" | Out-Null
    Require-MatterFile -Path (Join-Path $windowsKitsRoot "Include\$windowsSdkVersion\shared\winerror.h") -Description "Windows SDK $windowsSdkVersion shared headers" | Out-Null
    Require-MatterFile -Path (Join-Path $windowsKitsRoot "Lib\$windowsSdkVersion\um\x64\kernel32.lib") -Description "Windows SDK $windowsSdkVersion UM x64 import libraries" | Out-Null
    Require-MatterFile -Path (Join-Path $windowsKitsRoot "Lib\$windowsSdkVersion\ucrt\x64\ucrt.lib") -Description "Windows SDK $windowsSdkVersion UCRT x64 import libraries" | Out-Null

    $python = $null
    $pythonCandidates = @(
        (Join-Path $env:WINDIR 'py.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Launcher\py.exe'),
        (Get-Command py.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1)
    ) | Where-Object { $_ }
    foreach ($candidate in $pythonCandidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $python = $candidate
            break
        }
    }
    if (-not $python) {
        throw 'Python launcher py.exe was not found. Install Python 3.13.14 for all users so C:\Windows\py.exe is available.'
    }

    $vulkanSdk = 'C:\VulkanSDK\1.4.357.0'
    Require-MatterFile -Path (Join-Path $vulkanSdk 'Include\vulkan\vulkan.h') -Description 'Vulkan SDK 1.4.357.0 headers' | Out-Null
    Require-MatterFile -Path (Join-Path $vulkanSdk 'Lib\vulkan-1.lib') -Description 'Vulkan SDK 1.4.357.0 x64 import library' | Out-Null
    Require-MatterFile -Path (Join-Path $vulkanSdk 'Bin\VkLayer_khronos_validation.json') -Description 'Vulkan SDK 1.4.357.0 validation layer manifest' | Out-Null
    $glslc = Require-MatterFile -Path (Join-Path $vulkanSdk 'Bin\glslc.exe') -Description 'Vulkan SDK 1.4.357.0 glslc.exe'

    $result = [PSCustomObject][ordered]@{
        VisualStudioRoot = $visualStudioRoot
        VsDevCmd = $vsDevCmd
        MsvcToolsVersion = $msvcToolsVersion
        WindowsSdkVersion = $windowsSdkVersion
        CMake = $cmake
        Ninja = $ninja
        Python = $python
        VulkanSdk = $vulkanSdk
        Glslc = $glslc
    }

    if ($Json) {
        return ($result | ConvertTo-Json -Depth 2 -Compress)
    }

    return $result
}

Export-ModuleMember -Function Resolve-MatterWindowsToolchain
