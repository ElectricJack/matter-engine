$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$main = Get-Content -Raw (Join-Path $root 'MatterViewer\main.cpp')
$ui = Get-Content -Raw (Join-Path $root 'MatterViewer\ui.cpp')
$makefile = Get-Content -Raw (Join-Path $root 'MatterViewer\Makefile')
$engine = Get-Content -Raw (Join-Path $root 'MatterEngine3\include\matter\engine_context.h')
$world = Get-Content -Raw (Join-Path $root 'MatterEngine3\include\matter\world_session.h')
$vkContext = Get-Content -Raw (Join-Path $root 'MatterEngine3\src\render\vk_context.cpp')
$runtimeSmokePath = Join-Path $root 'MatterViewer\tools\smoke_vulkan_viewer.ps1'
$runtimeSmoke = if (Test-Path $runtimeSmokePath) { Get-Content -Raw $runtimeSmokePath } else { '' }
$failures = [System.Collections.Generic.List[string]]::new()

function Require-Text([string]$Text, [string]$Needle, [string]$Label) {
    if (-not $Text.Contains($Needle)) { $failures.Add("missing ${Label}: $Needle") }
}
function Forbid-Text([string]$Text, [string]$Needle, [string]$Label) {
    if ($Text.Contains($Needle)) { $failures.Add("forbidden ${Label}: $Needle") }
}

@('#include "raylib.h"', 'InitWindow(', 'BeginDrawing(', 'EndDrawing(',
  'LoadImageFromScreen(', 'CloseWindow(') | ForEach-Object {
    Forbid-Text $main $_ 'legacy viewer API'
}
@('GLFW_NO_API', 'VulkanDevice::create', 'VulkanFrame', 'begin_frame(',
  'end_frame(', 'MATTER_SCREENSHOT') | ForEach-Object {
    Require-Text $main $_ 'Vulkan viewer path'
}
Forbid-Text $ui 'imgui_impl_opengl3' 'OpenGL ImGui backend'
Require-Text $ui 'imgui_impl_vulkan' 'Vulkan ImGui backend'
Require-Text $ui 'ImGui_ImplGlfw_InitForVulkan' 'GLFW Vulkan ImGui init'
Require-Text $ui 'imgui_context_initialized_' 'partial ImGui context cleanup guard'
Require-Text $ui 'glfw_backend_initialized_' 'partial ImGui GLFW cleanup guard'
Require-Text $ui 'vulkan_backend_initialized_' 'partial ImGui Vulkan cleanup guard'
Require-Text $engine 'render_device' 'EngineDesc Vulkan seam'
Require-Text $world 'VulkanFrame' 'WorldSession frame contract'
Require-Text $world 'readback_swapchain_rgba8' 'presented screenshot API'

$windowsRecipe = $makefile.Substring($makefile.IndexOf('windows:'))
Require-Text $windowsRecipe '-lvulkan-1' 'Vulkan viewer link'
Require-Text $makefile 'imgui_impl_vulkan.cpp' 'Vulkan ImGui compilation'
Require-Text $makefile 'VULKAN=1' 'Vulkan feature manifest'
Require-Text $makefile 'OPENGL=0' 'OpenGL-disabled feature manifest'
Require-Text $makefile 'LINUX_APP_SRC = main_linux.cpp ui_linux.cpp camera_controller.cpp' 'preserved Linux viewer sources'
Require-Text $makefile 'IMGUI_SRC_LINUX' 'preserved Linux ImGui backend selection'
Require-Text $makefile 'imgui_impl_opengl3.cpp' 'preserved Linux OpenGL ImGui backend'
Require-Text $vkContext 'vkGetPhysicalDeviceFormatProperties' 'presentation format capability query'
Require-Text $vkContext 'VK_FORMAT_FEATURE_BLIT_SRC_BIT' 'HDR blit-source capability check'
Require-Text $vkContext 'VK_FORMAT_FEATURE_BLIT_DST_BIT' 'swapchain blit-destination capability check'
Require-Text $vkContext 'VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT' 'linear blit capability check'
Require-Text $runtimeSmoke 'MATTER_TEST_RESIZE' 'runtime resize exercise'
Require-Text $runtimeSmoke 'PNG signature' 'runtime PNG signature validation'
Require-Text $runtimeSmoke 'GetPixel' 'runtime nontrivial pixel validation'
Require-Text $runtimeSmoke 'validation errors' 'runtime validation-clean assertion'

if ($failures.Count -ne 0) {
    [Console]::Error.WriteLine('vulkan-viewer gate: FAIL')
    $failures | ForEach-Object { [Console]::Error.WriteLine("  - $_") }
    exit 1
}
Write-Output 'vulkan-viewer gate: PASS'
