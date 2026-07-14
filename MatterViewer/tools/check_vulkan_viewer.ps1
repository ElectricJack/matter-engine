$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$main = Get-Content -Raw (Join-Path $root 'MatterViewer\main.cpp')
$ui = Get-Content -Raw (Join-Path $root 'MatterViewer\ui.cpp')
$makefile = Get-Content -Raw (Join-Path $root 'MatterViewer\Makefile')
$engine = Get-Content -Raw (Join-Path $root 'MatterEngine3\include\matter\engine_context.h')
$world = Get-Content -Raw (Join-Path $root 'MatterEngine3\include\matter\world_session.h')
$vkContext = Get-Content -Raw (Join-Path $root 'MatterEngine3\src\render\vk_context.cpp')
$vkScene = Get-Content -Raw (Join-Path $root 'MatterEngine3\src\render\vk_scene_renderer.cpp')
$vkSceneHeader = Get-Content -Raw (Join-Path $root 'MatterEngine3\src\render\vk_scene_renderer.h')
$engineImpl = Get-Content -Raw (Join-Path $root 'MatterEngine3\src\matter_engine.cpp')
$vkSmoke = Get-Content -Raw (Join-Path $root 'MatterEngine3\tests\vulkan_smoke_tests.cpp')
$compositeShader = Get-Content -Raw (Join-Path $root 'MatterEngine3\shaders_vk\composite.frag')
$compat = Get-Content -Raw (Join-Path $root 'MatterEngine3\src\render\vulkan_only_compat.cpp')
$cell = Get-Content -Raw (Join-Path $root 'MatterSurfaceLib\src\cell.cpp')
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
Require-Text $main 'MATTER_HIDE_UI' 'runtime UI visibility option'
Require-Text $runtimeSmoke 'Assert-UiOverlay' 'runtime UI overlay assertion'
Require-Text $runtimeSmoke "'cornell-demo'" 'UI-visible demo capture'
Require-Text $runtimeSmoke "'cornell-materials'" 'UI-hidden normal material capture'
Require-Text $runtimeSmoke 'MATTER_HIDE_UI' 'UI-hidden scene verification'
Require-Text $runtimeSmoke 'PNG signature' 'runtime PNG signature validation'
Require-Text $runtimeSmoke 'GetPixel' 'runtime nontrivial pixel validation'
Require-Text $runtimeSmoke 'validation errors' 'runtime validation-clean assertion'

# Task 9 completion review: Vulkan world rendering must consume authored
# materials/lights and must not expose controls that are active no-ops.
Require-Text $engineImpl 'MaterialRegistryGet' 'Vulkan material registry lookup'
Require-Text $engineImpl 'mesh.texcoords[uv]' 'Vulkan material id channel'
Forbid-Text $engineImpl 'vertex.orm = {ao, 0.7f' 'invented Vulkan roughness'
Require-Text $vkSceneHeader 'VkSceneLighting' 'Vulkan scene lighting contract'
Require-Text $vkScene 'lighting_' 'Vulkan scene lighting state'
Require-Text $compositeShader 'sun_direction' 'world sun direction shading'
Require-Text $compositeShader 'sky_color' 'world sky lighting'
Forbid-Text $compositeShader 'max(normal.y' 'hard-coded normal-Y lighting'
Require-Text $ui 'not available in Vulkan milestone' 'disabled Vulkan milestone controls'

# Every CPU/legacy release site in the world facade must release the matching
# Vulkan part before its PartStore record can be reloaded.
$releaseSiteCount = ([regex]::Matches($engineImpl, 'store->release\(')).Count
$vkReleaseSiteCount = ([regex]::Matches($engineImpl, 'vk_scene->release_part\(')).Count
if ($vkReleaseSiteCount -lt $releaseSiteCount) {
    $failures.Add("Vulkan release symmetry: $vkReleaseSiteCount Vulkan releases for $releaseSiteCount PartStore releases")
}

# The Windows artifact is Vulkan-only. Source headers may still supply POD
# compatibility types, but the recipe/import table may not retain GL backends.
Require-Text $makefile '-DMATTER_VULKAN_ONLY' 'Vulkan-only Windows macro'
Forbid-Text $windowsRecipe '$(WIN_RAYLIB)' 'Windows raylib link'
Forbid-Text $windowsRecipe '-lopengl32' 'Windows OpenGL link'
Forbid-Text $windowsRecipe 'imgui_impl_opengl3.cpp' 'Windows OpenGL ImGui backend'
Require-Text $makefile 'objdump' 'PE import inspection'
@('opengl32','ChoosePixelFormat','SetPixelFormat','SwapBuffers','cuGraphicsGL') | ForEach-Object {
    Require-Text $makefile $_ 'forbidden PE import check'
}

# ImGui pipelines are render-format-specific. A resize that changes the
# swapchain format must rebuild the Vulkan backend, not only change image count.
Require-Text $ui 'frame.swapchain_format != swapchain_format_' 'ImGui format-change detection'
Require-Text $ui 'ImGui_ImplVulkan_Shutdown' 'ImGui format-change teardown'
Require-Text $ui 'ImGui_ImplVulkan_Init' 'ImGui format-change rebuild'
Require-Text $ui 'ImGui_ImplVulkan_SetMinImageCount' 'ImGui min image count update'
$preparePos = $ui.IndexOf('prepare_vulkan_backend(frame')
$newFramePos = $ui.IndexOf('ImGui_ImplVulkan_NewFrame')
if ($preparePos -lt 0 -or $newFramePos -lt 0 -or $preparePos -gt $newFramePos) {
    $failures.Add('ImGui Vulkan backend must be prepared before NewFrame creates draw data')
}
Forbid-Text $ui 'ImGui::Begin("RT Lighting")' 'active no-op RT lighting panel'
Require-Text $engineImpl 'MaterialRegistryPackForGPU' 'packed runtime material lookup'
Require-Text $engineImpl 'vulkan_material_uses_unsupported_texture' 'runtime texture override detection'
Require-Text $engineImpl 'ground material texture sampling is' 'unsupported texture warning'
Require-Text $engineImpl 'MATTER_VK_DIAGNOSTIC_GROUND_TILESET_PRIOR_SLOT' 'diagnostic prior-slot seed'
Require-Text $engineImpl 'prior_packed_slot' 'packed material slot snapshot'
Require-Text $engineImpl 'restored ground tileset material' 'exact packed material restoration log'
Require-Text $engineImpl 'vulkan_encode_emission' 'half-float authored emission encoding'
Require-Text $compositeShader 'normal_payload.w' 'normal-alpha authored emission decode'
Require-Text $vkSmoke 'max_center.hdr.x > thousand_center.hdr.x' 'strict saturated emission separation'
Require-Text $vkSmoke 'max_center.hdr.x > 14000.0f' 'saturated emission lower bound'
Require-Text $vkSmoke 'max_center.hdr.x < 16000.0f' 'saturated emission upper bound'
Require-Text $vkSmoke 'restore emission 5 before authored bright sky' 'identical-material sky-light comparison'
Require-Text $vkSmoke 'dark and bright sky samples keep identical G-buffer' 'identical sky-light test inputs'
Forbid-Text $compositeShader 'albedo.rgb * albedo.a' 'clamped albedo-alpha emission'
Forbid-Text $compositeShader 'orm.w' 'R8 ORM-alpha emission decode'
Require-Text $runtimeSmoke 'VK_LAYER_PATH' 'explicit validation layer discovery'
Forbid-Text $runtimeSmoke 'VK_LOADER_LAYERS_DISABLE' 'validation layer disable override'
Require-Text $runtimeSmoke 'red Cornell region' 'Cornell material assertion'
Require-Text $runtimeSmoke 'green Cornell region' 'Cornell material assertion'
Require-Text $runtimeSmoke 'gray Cornell region' 'Cornell material assertion'
Require-Text $runtimeSmoke 'Vulkan material diagnostic:' 'bake-to-RasterMeshData material assertion'
Require-Text $runtimeSmoke 'MATTER_VK_DIAGNOSTIC_GROUND_TILESET_MATERIAL' 'rendered packed-material override'
Require-Text $runtimeSmoke 'MATTER_VK_DIAGNOSTIC_GROUND_TILESET_PRIOR_SLOT' 'non-default prior packed-material slot'
Require-Text $runtimeSmoke 'seeded ground tileset material 8 prior packed slot 2' 'prior-slot seed assertion'
Require-Text $runtimeSmoke 'restored ground tileset material 8 to packed slot 2' 'exact packed-slot restoration assertion'
Require-Text $runtimeSmoke 'did not exercise the rendered packed-material warning' 'end-to-end texture warning assertion'
Require-Text $runtimeSmoke 'did not restore the exact packed prior material slot' 'diagnostic exact restoration assertion'
Require-Text $runtimeSmoke 'Get-FileHash' 'PNG hash assertion'
Require-Text $main 'CreateFile' 'Windows command file reader'
Forbid-Text $main 'MATTER_CMD_FIFO not supported on Windows' 'ignored Windows command interface'
Require-Text $main 'FATAL: MATTER_WORLD' 'explicit missing world failure'
Require-Text $main 'selected world' 'selected world identity report'
Require-Text $runtimeSmoke 'MATTER_CACHE_ROOT' 'isolated clean cache smoke'
Require-Text $runtimeSmoke 'selected world CornellBox hash' 'selected world hash assertion'

# Vulkan-only compatibility must own CPU cleanup and may not pretend unsupported
# GPU operations succeeded through empty bodies.
foreach ($field in @('vertices','texcoords','texcoords2','normals','tangents',
                      'colors','indices','animVertices','animNormals','boneIds',
                      'boneWeights','boneMatrices','vboId')) {
    Require-Text $compat "mesh.$field" "Mesh CPU cleanup for $field"
}
Forbid-Text $compat 'void UploadMesh(Mesh*, bool) {}' 'silent UploadMesh stub'
Forbid-Text $compat 'Texture2D LoadTextureFromImage(Image) { return Texture2D{}; }' 'silent texture stub'
Forbid-Text $compat 'int GetShaderLocation(Shader, const char*) { return -1; }' 'silent shader stub'
Require-Text $cell '#ifndef MATTER_VULKAN_ONLY' 'Vulkan-only UploadMesh exclusion'

foreach ($feature in @('CUDA_AVAILABLE=1','OPTIX_AVAILABLE=1','CUDA_ACTIVE=1',
                        'OPTIX_ACTIVE=0','VULKAN=1','OPENGL=0')) {
    Require-Text $makefile $feature 'truthful feature manifest'
}
Forbid-Text $makefile 'cuDevicePrimaryCtxRelease' 'artificial CUDA retention symbol'

if ($failures.Count -ne 0) {
    [Console]::Error.WriteLine('vulkan-viewer gate: FAIL')
    $failures | ForEach-Object { [Console]::Error.WriteLine("  - $_") }
    exit 1
}
Write-Output 'vulkan-viewer gate: PASS'
