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
$streamline = Get-Content -Raw (Join-Path $root 'MatterEngine3\src\render\streamline_bridge.cpp')
$compositeShader = Get-Content -Raw (Join-Path $root 'MatterEngine3\shaders_vk\composite.frag')
$compat = Get-Content -Raw (Join-Path $root 'MatterEngine3\src\render\vulkan_only_compat.cpp')
$cell = Get-Content -Raw (Join-Path $root 'MatterSurfaceLib\src\cell.cpp')
$runtimeSmokePath = Join-Path $root 'MatterViewer\tools\smoke_vulkan_viewer.ps1'
$runtimeSmoke = if (Test-Path $runtimeSmokePath) { Get-Content -Raw $runtimeSmokePath } else { '' }
$interopSmoke = Get-Content -Raw (Join-Path $root 'MatterViewer\tools\smoke_vulkan_interop_faults.ps1')
$perfScriptPath = Join-Path $root 'MatterViewer\tools\perf_vulkan_instancing.ps1'
$perfScript = if (Test-Path $perfScriptPath) { Get-Content -Raw $perfScriptPath } else { '' }
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

# RTX/DLSS final evidence: initialization must happen before Vulkan object
# creation, every presentation operation stays in the proxy funnel, and the
# exact temporal/output/RT ownership contracts retain executable coverage.
$deviceCreate = $vkContext.IndexOf('VulkanDevice::create(GLFWwindow* window')
$streamlineInit = $vkContext.IndexOf('StreamlineBridge::initialize_before_vulkan()', $deviceCreate)
$vulkanInit = $vkContext.IndexOf('result->impl_->initialize(window, enable_validation, error)', $deviceCreate)
if ($streamlineInit -lt 0 -or $vulkanInit -lt 0 -or $streamlineInit -gt $vulkanInit) {
    $failures.Add('Streamline manual hooking must initialize before Vulkan objects')
}
foreach ($hook in @('streamline.create_instance(', 'streamline.create_device(',
                     'streamline.create_swapchain(', 'streamline.get_swapchain_images(',
                     'streamline.acquire_next_image(', 'streamline.present_common(',
                     'streamline.queue_present(', 'streamline.destroy_swapchain(')) {
    Require-Text $vkContext $hook 'Streamline proxy funnel'
}
if (([regex]::Matches($vkContext, 'streamline\.present_common\(input\.serial\)')).Count -ne 1 -or
    ([regex]::Matches($vkContext, 'streamline\.queue_present\(graphics_queue')).Count -ne 1) {
    $failures.Add('successful frame must have exactly one common-present and one queue-present site')
}
$commonPresent = $vkContext.IndexOf('streamline.present_common(input.serial)')
$queuePresent = $vkContext.IndexOf('streamline.queue_present(graphics_queue')
if ($commonPresent -lt 0 -or $queuePresent -lt 0 -or $commonPresent -gt $queuePresent) {
    $failures.Add('common-present must immediately precede the sole proxied queue-present')
}
@('static camera and rigid instance produce zero temporal velocity',
  'velocity attachment stores exact current-to-previous input pixels',
  'dlss_output_evaluated', 'first_dlss_output',
  'test_rt_geometry_address(910) == pinned',
  'test_rt_blas_candidate_serial(920) == frame.serial') | ForEach-Object {
    Require-Text $vkSmoke $_ 'RTX/DLSS executable evidence hook'
}
Require-Text $streamline 'sl.interposer.dll is missing beside the executable' 'truthful Streamline runtime absence'
Require-Text $runtimeSmoke 'DLSS selected=Native active=Native' 'runtime Native fallback assertion'
Require-Text $runtimeSmoke 'MATTER_DISABLE_VK_RT' 'runtime RT toggle assertion'
foreach ($mode in @("'rt'", "'rt-disabled'", "'rt-unavailable'")) {
    Require-Text $interopSmoke $mode 'executable final RT smoke mode'
}
Require-Text $interopSmoke 'System.Diagnostics.ProcessStartInfo' 'duplicate-safe smoke process launch'
Require-Text $interopSmoke 'EnvironmentVariables.Clear()' 'sanitized smoke environment'
Require-Text $interopSmoke 'OrdinalIgnoreCase' 'case-insensitive smoke environment deduplication'
Forbid-Text $interopSmoke 'Start-Process' 'duplicate-sensitive smoke process launch'
@('vk_rt_available', 'vk_rt_effective', 'vk_rt_trace_dispatches',
  'vk_rt_fallback_reason') | ForEach-Object {
    Require-Text $world $_ 'renderer-observed RT FrameStats'
    Require-Text $main $_ 'renderer-observed RT JSON evidence'
    Require-Text $perfScript $_ 'renderer-observed RT performance gate'
}
Require-Text $runtimeSmoke 'Vulkan RT observed effective=' 'renderer-observed viewer smoke assertion'
Require-Text $engineImpl 'world session disconnected' 'disconnected-frame RT fallback reason'
Require-Text $engineImpl 'world part store unavailable' 'missing-store RT fallback reason'
Require-Text $vkScene 'last_rt_samples_ = ray_tracing_settings_.samples' 'per-frame observed RT sample reset'
Require-Text $vkScene 'last_rt_debug_view_ = ray_tracing_settings_.debug_view' 'per-frame observed RT debug reset'

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

# GPU-instancing parity performance contract. The viewer owns the deterministic
# interval sampling and the PowerShell harness enforces its measured output;
# retain every counter field so a future change cannot silently drop a guard.
@('MATTER_PERF_OUTPUT', 'MATTER_PERF_WARMUP_SECONDS',
  'MATTER_PERF_SAMPLE_SECONDS') | ForEach-Object {
    Require-Text $main $_ 'performance environment contract'
    Require-Text $perfScript $_ 'performance harness environment'
}
@('frame_metric', 'median_frame_ms', 'median_fps', 'p95_frame_ms', 'static_vertex_upload_delta',
  'static_cluster_upload_delta', 'stable_instance_upload_delta',
  'immediate_submit_delta', 'validation_errors') | ForEach-Object {
    Require-Text $main $_ 'performance JSON counter'
}
@('selected_dlss_mode', 'active_dlss_mode', 'dlss_internal_width',
  'dlss_internal_height', 'dlss_output_width', 'dlss_output_height',
  'dlss_reset_delta', 'rt_available', 'rt_enabled', 'rt_samples',
  'rt_debug_view', 'fallback_reason') | ForEach-Object {
    Require-Text $main $_ 'RTX/DLSS performance JSON evidence'
    Require-Text $perfScript $_ 'RTX/DLSS performance gate'
}
Require-Text $main 'end_to_end_cadence' 'end-to-end performance metric label'
Require-Text $main 'perf_frame_cadence_ms' 'end-to-end performance frame sample'
Forbid-Text $main 'perf_frame_times.push_back(stats.frame_ms)' 'CPU-only render timing sample'
$perfFrameStart = $main.IndexOf('const auto perf_frame_start = std::chrono::steady_clock::now();')
$pollEvents = $main.IndexOf('glfwPollEvents();')
$endFrame = $main.IndexOf('vulkan->end_frame(frame, frame_presented, error)')
$perfFrameSample = $main.IndexOf('perf_frame_times.push_back(perf_frame_cadence_ms)')
if ($perfFrameStart -lt 0 -or $pollEvents -lt 0 -or $endFrame -lt 0 -or
    $perfFrameSample -lt 0 -or $perfFrameStart -gt $pollEvents -or
    $perfFrameSample -lt $endFrame) {
    $failures.Add('performance samples must span viewer pacing, begin_frame, and end_frame/present')
}
@('Get-ChildItem Env:', "Name -like 'MATTER_*'", 'MATTER_CACHE_ROOT',
  'finally', 'Remove-Item -LiteralPath $runRoot',
  'Write-Output (("Vulkan instancing performance:') | ForEach-Object {
    Require-Text $perfScript $_ 'isolated performance harness state'
}
Require-Text $perfScript 'VK_LAYER_PATH' 'performance validation layer discovery'
Require-Text $makefile 'vulkan-instancing-perf: windows' 'performance Make target'
Require-Text $vkScene 'multi_draw_indirect_enabled' 'grouped indirect feature gate'
Require-Text $vkScene 'range.command_count' 'grouped indirect draw ranges'
Require-Text $vkScene 'vkCmdDrawIndirect(command_buffer, record.indirect_buffer' 'grouped indirect recording'
$recordStart = $vkScene.IndexOf('bool VkSceneRenderer::record_cull_and_render(')
$recordEnd = if ($recordStart -ge 0) {
    $vkScene.IndexOf('#ifdef MATTER_VK_TEST_FAULT_INJECTION', $recordStart)
} else { -1 }
if ($recordStart -lt 0 -or $recordEnd -lt $recordStart) {
    $failures.Add('could not isolate the production Vulkan world-render record path')
} else {
    $recordPath = $vkScene.Substring($recordStart, $recordEnd - $recordStart)
    Forbid-Text $recordPath 'submit_immediate(' 'production Vulkan world-render submission'
}

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
