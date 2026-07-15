#include "streamline_bridge.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <utility>

#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
#include <windows.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wconversion-null"
#endif
#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers_vk.h>
#include <sl_security.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

namespace matter {
namespace {

template <typename Features>
void merge_feature_bits(Features& destination, const Features& source) {
    // Both Vulkan core feature structs start with sType and pNext; every
    // remaining member is VkBool32.  Preserve the caller-owned pNext chain.
    constexpr size_t kFirstFeature = sizeof(VkBaseOutStructure);
    static_assert(sizeof(Features) >= kFirstFeature);
    auto* destination_bits = reinterpret_cast<VkBool32*>(
        reinterpret_cast<unsigned char*>(&destination) + kFirstFeature);
    const auto* source_bits = reinterpret_cast<const VkBool32*>(
        reinterpret_cast<const unsigned char*>(&source) + kFirstFeature);
    constexpr size_t kFeatureCount =
        (sizeof(Features) - kFirstFeature) / sizeof(VkBool32);
    for (size_t i = 0; i < kFeatureCount; ++i) {
        destination_bits[i] = destination_bits[i] || source_bits[i]
                                  ? VK_TRUE
                                  : VK_FALSE;
    }
}

template <typename Features>
bool required_feature_bits_supported(const Features& required,
                                    const Features& supported) {
    constexpr size_t kFirstFeature = sizeof(VkBaseOutStructure);
    const auto* required_bits = reinterpret_cast<const VkBool32*>(
        reinterpret_cast<const unsigned char*>(&required) + kFirstFeature);
    const auto* supported_bits = reinterpret_cast<const VkBool32*>(
        reinterpret_cast<const unsigned char*>(&supported) + kFirstFeature);
    constexpr size_t kFeatureCount =
        (sizeof(Features) - kFirstFeature) / sizeof(VkBool32);
    for (size_t i = 0; i < kFeatureCount; ++i) {
        if (required_bits[i] && !supported_bits[i]) return false;
    }
    return true;
}

#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
std::wstring interposer_path() {
    std::array<wchar_t, MAX_PATH> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                             executable.size());
    if (length == 0 || length >= executable.size()) return {};
    std::wstring path(executable.data(), length);
    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return {};
    path.resize(separator + 1);
    path += L"sl.interposer.dll";
    return path;
}

std::string result_reason(const char* operation, sl::Result result) {
    std::ostringstream out;
    out << operation << " failed (Streamline result "
        << static_cast<int>(result) << ")";
    return out.str();
}

using SlInitFn = sl::Result (*)(const sl::Preferences&, uint64_t);
using SlGetFeatureRequirementsFn =
    sl::Result (*)(sl::Feature, sl::FeatureRequirements&);
using SlSetVulkanInfoFn = sl::Result (*)(const sl::VulkanInfo&);
using SlShutdownFn = sl::Result (*)();
using SlIsFeatureSupportedFn = PFun_slIsFeatureSupported*;
using SlEvaluateFeatureFn = PFun_slEvaluateFeature*;
using SlSetTagForFrameFn = PFun_slSetTagForFrame*;
using SlSetConstantsFn = PFun_slSetConstants*;
using SlGetFeatureFunctionFn = PFun_slGetFeatureFunction*;
using SlGetNewFrameTokenFn = PFun_slGetNewFrameToken*;
using SlDlssGetOptimalSettingsFn = PFun_slDLSSGetOptimalSettings*;
using SlDlssSetOptionsFn = PFun_slDLSSSetOptions*;

template <typename Function>
Function streamline_function(HMODULE module, const char* name) {
    const FARPROC address = GetProcAddress(module, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

sl::DLSSMode streamline_dlss_mode(DlssMode mode) {
    switch (mode) {
        case DlssMode::Quality: return sl::DLSSMode::eMaxQuality;
        case DlssMode::Balanced: return sl::DLSSMode::eBalanced;
        case DlssMode::Performance: return sl::DLSSMode::eMaxPerformance;
        case DlssMode::Native: return sl::DLSSMode::eOff;
    }
    return sl::DLSSMode::eOff;
}

sl::DLSSOptions streamline_dlss_options(const DlssOptions& options) {
    sl::DLSSOptions converted{};
    converted.mode = streamline_dlss_mode(options.mode);
    converted.outputWidth = options.output_extent.width;
    converted.outputHeight = options.output_extent.height;
    converted.colorBuffersHDR = options.color_buffers_hdr
                                    ? sl::Boolean::eTrue
                                    : sl::Boolean::eFalse;
    converted.useAutoExposure = options.use_auto_exposure
                                    ? sl::Boolean::eTrue
                                    : sl::Boolean::eFalse;
    return converted;
}

void copy_matrix(sl::float4x4& destination, const float source[16]) {
    for (uint32_t row = 0; row < 4; ++row) {
        destination[row] = {source[row * 4], source[row * 4 + 1],
                            source[row * 4 + 2], source[row * 4 + 3]};
    }
}

template <typename Handle>
void* vulkan_handle(Handle handle) {
    static_assert(sizeof(Handle) <= sizeof(uintptr_t));
    uintptr_t bits = 0;
    std::memcpy(&bits, &handle, sizeof(handle));
    return reinterpret_cast<void*>(bits);
}
#endif

}  // namespace

StreamlineBridge StreamlineBridge::initialize_before_vulkan() {
    StreamlineBridge bridge;
    bridge.initialized_ = true;
#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
    const std::wstring dll_path = interposer_path();
    if (dll_path.empty() || GetFileAttributesW(dll_path.c_str()) ==
                                 INVALID_FILE_ATTRIBUTES) {
        bridge.dlss_unavailable_reason_ =
            "Streamline SDK not found: sl.interposer.dll is missing beside the executable";
        return bridge;
    }
    if (!sl::security::verifyEmbeddedSignature(dll_path.c_str())) {
        bridge.dlss_unavailable_reason_ =
            "Streamline SDK signature verification failed for sl.interposer.dll";
        return bridge;
    }
    const HMODULE module = LoadLibraryW(dll_path.c_str());
    if (!module) {
        bridge.dlss_unavailable_reason_ =
            "Streamline SDK could not load the signed sl.interposer.dll";
        return bridge;
    }
    bridge.streamline_module_ = module;
    std::fprintf(stderr, "Streamline: signed interposer loaded\n");
    bridge.sl_init_ = reinterpret_cast<void*>(
        streamline_function<SlInitFn>(module, "slInit"));
    bridge.sl_get_feature_requirements_ = reinterpret_cast<void*>(
        streamline_function<SlGetFeatureRequirementsFn>(
            module, "slGetFeatureRequirements"));
    bridge.sl_set_vulkan_info_ = reinterpret_cast<void*>(
        streamline_function<SlSetVulkanInfoFn>(module, "slSetVulkanInfo"));
    bridge.sl_shutdown_ = reinterpret_cast<void*>(
        streamline_function<SlShutdownFn>(module, "slShutdown"));
    bridge.sl_is_feature_supported_ = reinterpret_cast<void*>(
        streamline_function<SlIsFeatureSupportedFn>(module,
                                                     "slIsFeatureSupported"));
    bridge.sl_evaluate_feature_ = reinterpret_cast<void*>(
        streamline_function<SlEvaluateFeatureFn>(module, "slEvaluateFeature"));
    bridge.sl_set_tag_for_frame_ = reinterpret_cast<void*>(
        streamline_function<SlSetTagForFrameFn>(module, "slSetTagForFrame"));
    bridge.sl_set_constants_ = reinterpret_cast<void*>(
        streamline_function<SlSetConstantsFn>(module, "slSetConstants"));
    bridge.sl_get_feature_function_ = reinterpret_cast<void*>(
        streamline_function<SlGetFeatureFunctionFn>(module,
                                                     "slGetFeatureFunction"));
    bridge.sl_get_new_frame_token_ = reinterpret_cast<void*>(
        streamline_function<SlGetNewFrameTokenFn>(module,
                                                   "slGetNewFrameToken"));
    if (!bridge.sl_init_ || !bridge.sl_get_feature_requirements_ ||
        !bridge.sl_set_vulkan_info_ || !bridge.sl_shutdown_ ||
        !bridge.sl_is_feature_supported_ || !bridge.sl_evaluate_feature_ ||
        !bridge.sl_set_tag_for_frame_ || !bridge.sl_set_constants_ ||
        !bridge.sl_get_feature_function_ || !bridge.sl_get_new_frame_token_) {
        bridge.dlss_unavailable_reason_ =
            "Streamline SDK is missing a required exported entry point";
        FreeLibrary(module);
        bridge.streamline_module_ = nullptr;
        return bridge;
    }
    bridge.get_instance_proc_addr_proxy_ =
        streamline_function<PFN_vkGetInstanceProcAddr>(
            module, "vkGetInstanceProcAddr");
    bridge.get_device_proc_addr_proxy_ =
        streamline_function<PFN_vkGetDeviceProcAddr>(
            module, "vkGetDeviceProcAddr");
    if (!bridge.get_instance_proc_addr_proxy_ ||
        !bridge.get_device_proc_addr_proxy_) {
        bridge.disable("Streamline SDK is missing Vulkan manual-hook dispatchers");
        return bridge;
    }
    const std::array<sl::Feature, 1> features = {sl::kFeatureDLSS};
    sl::Preferences preferences{};
    preferences.featuresToLoad = features.data();
    preferences.numFeaturesToLoad = static_cast<uint32_t>(features.size());
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "MatterEngine3 Vulkan 1.0";
    preferences.projectId = "26d45b4e-66d2-4fe9-8f3e-76758cad7d6d";
    preferences.flags |= sl::PreferenceFlags::eUseManualHooking |
                         sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    const sl::Result init_result =
        reinterpret_cast<SlInitFn>(bridge.sl_init_)(preferences, sl::kSDKVersion);
    if (init_result != sl::Result::eOk) {
        bridge.disable(result_reason("slInit", init_result));
        return bridge;
    }
    bridge.streamline_initialized_ = true;
    sl::FeatureRequirements requirements{};
    const sl::Result requirements_result =
        reinterpret_cast<SlGetFeatureRequirementsFn>(
            bridge.sl_get_feature_requirements_)(sl::kFeatureDLSS, requirements);
    if (requirements_result != sl::Result::eOk) {
        bridge.disable(
            result_reason("slGetFeatureRequirements(DLSS)", requirements_result));
        return bridge;
    }
    bridge.dlss_requested_ = true;
    bridge.dlss_available_ = true;
    bridge.native_retry_required_ = true;
    bridge.instance_extensions_.assign(
        requirements.vkInstanceExtensions,
        requirements.vkInstanceExtensions + requirements.vkNumInstanceExtensions);
    bridge.device_extensions_.assign(
        requirements.vkDeviceExtensions,
        requirements.vkDeviceExtensions + requirements.vkNumDeviceExtensions);
    bridge.required_features12_ = sl::getVkPhysicalDeviceVulkan12Features(
        requirements.vkNumFeatures12, requirements.vkFeatures12);
    bridge.required_features13_ = sl::getVkPhysicalDeviceVulkan13Features(
        requirements.vkNumFeatures13, requirements.vkFeatures13);
    bridge.additional_graphics_queues_ = requirements.vkNumGraphicsQueuesRequired;
    bridge.additional_compute_queues_ = requirements.vkNumComputeQueuesRequired;
    bridge.use_proxy_dispatch_ = true;
#else
    bridge.dlss_unavailable_reason_ =
        "Streamline SDK not found: build with HAVE_STREAMLINE=1 to enable DLSS";
#endif
    return bridge;
}

StreamlineBridge StreamlineBridge::native_fallback(std::string reason) {
    StreamlineBridge bridge;
    bridge.initialized_ = true;
    bridge.dlss_unavailable_reason_ = std::move(reason);
    return bridge;
}

const char* dlss_mode_name(DlssMode mode) noexcept {
    switch (mode) {
        case DlssMode::Native: return "Native";
        case DlssMode::Quality: return "Quality";
        case DlssMode::Balanced: return "Balanced";
        case DlssMode::Performance: return "Performance";
    }
    return "Native";
}

bool StreamlineBridge::query_dlss_optimal_settings(
    const DlssOptions& options, DlssOptimalSettings& settings,
    std::string& error) const {
    error.clear();
    settings = {};
    if (options.output_extent.width == 0 || options.output_extent.height == 0) {
        error = "DLSS optimal settings require a nonzero output extent";
        return false;
    }
    if (options.mode == DlssMode::Native) {
        settings.render_extent = options.output_extent;
        return true;
    }
    if (!dlss_available_) {
        error = dlss_unavailable_reason_;
        return false;
    }
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (test_dlss_optimal_evaluator_ &&
        test_dlss_optimal_evaluator_(options, settings, error) &&
        settings.render_extent.width != 0 &&
        settings.render_extent.height != 0) {
        return true;
    }
#endif
#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
    if (sl_dlss_get_optimal_settings_) {
        const sl::DLSSOptions streamline_options =
            streamline_dlss_options(options);
        sl::DLSSOptimalSettings optimal{};
        const sl::Result result =
            reinterpret_cast<SlDlssGetOptimalSettingsFn>(
                sl_dlss_get_optimal_settings_)(streamline_options, optimal);
        if (result == sl::Result::eOk && optimal.optimalRenderWidth != 0 &&
            optimal.optimalRenderHeight != 0) {
            settings.render_extent = {optimal.optimalRenderWidth,
                                      optimal.optimalRenderHeight};
            settings.sharpness = optimal.optimalSharpness;
            return true;
        }
        error = result_reason("slDLSSGetOptimalSettings", result);
    }
#endif
    if (error.empty())
        error = "Streamline DLSS optimal settings are unavailable in this build";
    settings = {};
    return false;
}

bool StreamlineBridge::evaluate_dlss(
    VkCommandBuffer command_buffer, uint64_t attempt_token,
    const DlssOptions& options, const DlssConstants& constants,
    const DlssResources& resources, DlssEvaluationOutput& output,
    std::string& error) {
    error.clear();
    output = {};
    if (options.mode == DlssMode::Native) {
        if (active_dlss_mode_ == DlssMode::Native) return true;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        if (test_dlss_evaluator_) {
            ++test_dlss_evaluation_count_;
            if (!test_dlss_evaluator_(command_buffer, attempt_token, options,
                                      constants, resources, output, error)) {
                if (error.empty()) error = "DLSS Native transition failed";
                active_dlss_mode_ = DlssMode::Native;
                dlss_history_reset_pending_ = true;
                dlss_available_ = false;
                dlss_requested_ = false;
                dlss_unavailable_reason_ = error;
                return false;
            }
        } else
#endif
        {
#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
            if (dlss_available_ && sl_dlss_set_options_) {
                const sl::DLSSOptions streamline_options =
                    streamline_dlss_options(options);
                const sl::Result result =
                    reinterpret_cast<SlDlssSetOptionsFn>(
                        sl_dlss_set_options_)(sl::ViewportHandle(0u),
                                              streamline_options);
                if (result != sl::Result::eOk) {
                    error = result_reason("slDLSSSetOptions(eOff)", result);
                    active_dlss_mode_ = DlssMode::Native;
                    dlss_history_reset_pending_ = true;
                    dlss_available_ = false;
                    dlss_requested_ = false;
                    dlss_unavailable_reason_ = error;
                    return false;
                }
            }
#endif
        }
        // Retain feature/output allocations for fast toggles. They remain
        // device-owned and are released during normal renderer/SL teardown.
        active_dlss_mode_ = DlssMode::Native;
        dlss_history_reset_pending_ = true;
        return true;
    }
    const bool valid_extents = constants.internal_extent.width != 0 &&
                               constants.internal_extent.height != 0 &&
                               constants.output_extent.width != 0 &&
                               constants.output_extent.height != 0 &&
                               constants.output_extent.width ==
                                   options.output_extent.width &&
                               constants.output_extent.height ==
                                   options.output_extent.height;
    const bool distinct_resources =
        resources.hdr.image != VK_NULL_HANDLE &&
        resources.depth.image != VK_NULL_HANDLE &&
        resources.velocity.image != VK_NULL_HANDLE &&
        resources.output.image != VK_NULL_HANDLE &&
        resources.hdr.image != resources.depth.image &&
        resources.hdr.image != resources.velocity.image &&
        resources.hdr.image != resources.output.image &&
        resources.depth.image != resources.velocity.image &&
        resources.depth.image != resources.output.image &&
        resources.velocity.image != resources.output.image;
    const bool exact_resource_contract =
        resources.hdr.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
        resources.depth.format == VK_FORMAT_D32_SFLOAT &&
        resources.velocity.format == VK_FORMAT_R16G16_SFLOAT &&
        resources.output.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
        resources.hdr.extent.width == constants.internal_extent.width &&
        resources.hdr.extent.height == constants.internal_extent.height &&
        resources.depth.extent.width == constants.internal_extent.width &&
        resources.depth.extent.height == constants.internal_extent.height &&
        resources.velocity.extent.width == constants.internal_extent.width &&
        resources.velocity.extent.height == constants.internal_extent.height &&
        resources.output.extent.width == options.output_extent.width &&
        resources.output.extent.height == options.output_extent.height &&
        resources.hdr.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        resources.depth.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        resources.velocity.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        resources.output.layout == VK_IMAGE_LAYOUT_GENERAL &&
        resources.hdr.stage == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
        resources.depth.stage == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
        resources.velocity.stage == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
        resources.output.stage == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
        resources.hdr.access == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT &&
        resources.depth.access == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT &&
        resources.velocity.access == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT &&
        resources.output.access == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    const bool complete_vulkan_resources =
        resources.hdr.view != VK_NULL_HANDLE &&
        resources.hdr.memory != VK_NULL_HANDLE &&
        resources.hdr.aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
        resources.hdr.usage != 0 && resources.depth.view != VK_NULL_HANDLE &&
        resources.depth.memory != VK_NULL_HANDLE &&
        resources.depth.aspect == VK_IMAGE_ASPECT_DEPTH_BIT &&
        resources.depth.usage != 0 &&
        resources.velocity.view != VK_NULL_HANDLE &&
        resources.velocity.memory != VK_NULL_HANDLE &&
        resources.velocity.aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
        resources.velocity.usage != 0 &&
        resources.output.view != VK_NULL_HANDLE &&
        resources.output.memory != VK_NULL_HANDLE &&
        resources.output.aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
        (resources.output.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    if (!dlss_available_ || !valid_extents || !distinct_resources ||
        !exact_resource_contract || !complete_vulkan_resources ||
        !constants.motion_vectors_jittered ||
        !options.color_buffers_hdr || !options.use_auto_exposure) {
        error = dlss_available_
                    ? "DLSS evaluation received invalid constants or resources"
                    : dlss_unavailable_reason_;
    } else {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        if (test_dlss_evaluator_) {
            ++test_dlss_evaluation_count_;
            if (test_dlss_evaluator_(command_buffer, attempt_token, options,
                                     constants, resources, output, error) &&
                output.output_written &&
                output.layout != VK_IMAGE_LAYOUT_UNDEFINED &&
                output.stage != VK_PIPELINE_STAGE_2_NONE &&
                output.access != VK_ACCESS_2_NONE) {
                active_dlss_mode_ = options.mode;
                return true;
            }
        } else
#endif
        {
#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
            sl::FrameToken* frame_token = nullptr;
            sl::ViewportHandle viewport(0u);
            const sl::Result token_result =
                reinterpret_cast<SlGetNewFrameTokenFn>(
                    sl_get_new_frame_token_)(frame_token, nullptr);
            if (token_result != sl::Result::eOk || !frame_token) {
                error = result_reason("slGetNewFrameToken", token_result);
            } else {
                const sl::DLSSOptions streamline_options =
                    streamline_dlss_options(options);
                sl::Constants streamline_constants{};
                copy_matrix(streamline_constants.cameraViewToClip,
                            constants.camera_view_to_clip);
                copy_matrix(streamline_constants.clipToCameraView,
                            constants.clip_to_camera_view);
                copy_matrix(streamline_constants.clipToPrevClip,
                            constants.clip_to_prev_clip);
                copy_matrix(streamline_constants.prevClipToClip,
                            constants.prev_clip_to_clip);
                streamline_constants.jitterOffset = {
                    constants.jitter_offset.x, constants.jitter_offset.y};
                streamline_constants.mvecScale = {
                    constants.motion_vector_scale.x,
                    constants.motion_vector_scale.y};
                streamline_constants.cameraPos = {
                    constants.camera_position[0], constants.camera_position[1],
                    constants.camera_position[2]};
                streamline_constants.cameraUp = {
                    constants.camera_up[0], constants.camera_up[1],
                    constants.camera_up[2]};
                streamline_constants.cameraRight = {
                    constants.camera_right[0], constants.camera_right[1],
                    constants.camera_right[2]};
                streamline_constants.cameraFwd = {
                    constants.camera_forward[0], constants.camera_forward[1],
                    constants.camera_forward[2]};
                streamline_constants.cameraNear = constants.camera_near;
                streamline_constants.cameraFar = constants.camera_far;
                streamline_constants.cameraFOV = constants.camera_fov;
                streamline_constants.cameraAspectRatio =
                    constants.camera_aspect_ratio;
                streamline_constants.depthInverted = constants.depth_inverted
                                                         ? sl::Boolean::eTrue
                                                         : sl::Boolean::eFalse;
                streamline_constants.cameraMotionIncluded =
                    constants.camera_motion_included ? sl::Boolean::eTrue
                                                     : sl::Boolean::eFalse;
                streamline_constants.motionVectors3D = sl::Boolean::eFalse;
                streamline_constants.reset = constants.reset
                                                 ? sl::Boolean::eTrue
                                                 : sl::Boolean::eFalse;
                streamline_constants.motionVectorsJittered =
                    constants.motion_vectors_jittered ? sl::Boolean::eTrue
                                                      : sl::Boolean::eFalse;

                const auto make_resource = [](const DlssResource& source) {
                    sl::Resource resource(sl::ResourceType::eTex2d,
                                          vulkan_handle(source.image),
                                          vulkan_handle(source.memory),
                                          vulkan_handle(source.view),
                                          static_cast<uint32_t>(source.layout));
                    resource.width = source.extent.width;
                    resource.height = source.extent.height;
                    resource.nativeFormat = static_cast<uint32_t>(source.format);
                    resource.mipLevels = 1;
                    resource.arrayLayers = 1;
                    resource.usage = source.usage;
                    return resource;
                };
                sl::Resource hdr = make_resource(resources.hdr);
                sl::Resource depth = make_resource(resources.depth);
                sl::Resource velocity = make_resource(resources.velocity);
                sl::Resource dlss_output = make_resource(resources.output);
                sl::SubresourceRange hdr_range{};
                hdr_range.aspectMask = resources.hdr.aspect;
                hdr_range.baseMipLevel = 0;
                hdr_range.levelCount = 1;
                hdr_range.baseArrayLayer = 0;
                hdr_range.layerCount = 1;
                sl::SubresourceRange depth_range{};
                depth_range.aspectMask = resources.depth.aspect;
                depth_range.baseMipLevel = 0;
                depth_range.levelCount = 1;
                depth_range.baseArrayLayer = 0;
                depth_range.layerCount = 1;
                sl::SubresourceRange velocity_range{};
                velocity_range.aspectMask = resources.velocity.aspect;
                velocity_range.baseMipLevel = 0;
                velocity_range.levelCount = 1;
                velocity_range.baseArrayLayer = 0;
                velocity_range.layerCount = 1;
                sl::SubresourceRange output_range{};
                output_range.aspectMask = resources.output.aspect;
                output_range.baseMipLevel = 0;
                output_range.levelCount = 1;
                output_range.baseArrayLayer = 0;
                output_range.layerCount = 1;
                hdr.next = &hdr_range;
                depth.next = &depth_range;
                velocity.next = &velocity_range;
                dlss_output.next = &output_range;
                const sl::Extent input_extent{0, 0,
                                              constants.internal_extent.width,
                                              constants.internal_extent.height};
                const sl::Extent output_extent{0, 0,
                                               constants.output_extent.width,
                                               constants.output_extent.height};
                const std::array<sl::ResourceTag, 4> tags = {
                    sl::ResourceTag(&hdr, sl::kBufferTypeScalingInputColor,
                                    sl::ResourceLifecycle::eValidUntilEvaluate,
                                    &input_extent),
                    sl::ResourceTag(&depth, sl::kBufferTypeDepth,
                                    sl::ResourceLifecycle::eValidUntilEvaluate,
                                    &input_extent),
                    sl::ResourceTag(&velocity, sl::kBufferTypeMotionVectors,
                                    sl::ResourceLifecycle::eValidUntilEvaluate,
                                    &input_extent),
                    sl::ResourceTag(&dlss_output,
                                    sl::kBufferTypeScalingOutputColor,
                                    sl::ResourceLifecycle::eValidUntilEvaluate,
                                    &output_extent)};
                auto* sl_command_buffer = static_cast<sl::CommandBuffer*>(
                    vulkan_handle(command_buffer));
                sl::Result result =
                    reinterpret_cast<SlDlssSetOptionsFn>(
                        sl_dlss_set_options_)(viewport, streamline_options);
                if (result == sl::Result::eOk)
                    result = reinterpret_cast<SlSetConstantsFn>(
                        sl_set_constants_)(streamline_constants, *frame_token,
                                           viewport);
                if (result == sl::Result::eOk)
                    result = reinterpret_cast<SlSetTagForFrameFn>(
                        sl_set_tag_for_frame_)(
                        *frame_token, viewport, tags.data(),
                        static_cast<uint32_t>(tags.size()), sl_command_buffer);
                if (result == sl::Result::eOk) {
                    const sl::BaseStructure* evaluate_inputs[] = {&viewport};
                    result = reinterpret_cast<SlEvaluateFeatureFn>(
                        sl_evaluate_feature_)(sl::kFeatureDLSS, *frame_token,
                                              evaluate_inputs,
                                              1u,
                                              sl_command_buffer);
                }
                if (result == sl::Result::eOk) {
                    output = {true, VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              VK_ACCESS_2_MEMORY_WRITE_BIT};
                    active_dlss_mode_ = options.mode;
                    return true;
                }
                error = result_reason("Streamline DLSS evaluation", result);
            }
#else
            error = "Streamline DLSS evaluation is unavailable in this build";
#endif
        }
    }
    if (error.empty()) error = "DLSS evaluation failed";
    active_dlss_mode_ = DlssMode::Native;
    dlss_history_reset_pending_ = true;
    dlss_available_ = false;
    dlss_requested_ = false;
    dlss_unavailable_reason_ = error;
    return false;
}

bool StreamlineBridge::consume_dlss_history_reset() {
    const bool reset = dlss_history_reset_pending_;
    dlss_history_reset_pending_ = false;
    return reset;
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
StreamlineBridge StreamlineBridge::test_fake_dlss(
    TestDlssEvaluator evaluator,
    TestDlssOptimalEvaluator optimal_evaluator) {
    StreamlineBridge bridge;
    bridge.initialized_ = true;
    bridge.dlss_requested_ = true;
    bridge.dlss_available_ = true;
    bridge.test_dlss_evaluator_ = std::move(evaluator);
    bridge.test_dlss_optimal_evaluator_ = std::move(optimal_evaluator);
    return bridge;
}

StreamlineBridge StreamlineBridge::test_missing_proxy(const char* stage) {
    StreamlineBridge bridge;
    bridge.initialized_ = true;
    bridge.dlss_requested_ = true;
    bridge.dlss_available_ = true;
    bridge.use_proxy_dispatch_ = true;
    bridge.native_retry_required_ = true;
    if (stage && std::strcmp(stage, "instance") == 0)
        bridge.test_proxy_fault_ = TestProxyFault::Instance;
    else if (stage && std::strcmp(stage, "device") == 0)
        bridge.test_proxy_fault_ = TestProxyFault::Device;
    return bridge;
}
#endif

#ifdef MATTER_VK_TEST_FAULT_INJECTION
void StreamlineBridge::record_test_presentation_event(const char* event) {
    test_presentation_events_.emplace_back(event);
}
#endif

bool StreamlineBridge::validate_requirements(
    const VkPhysicalDeviceVulkan12Features& supported_features12,
    const VkPhysicalDeviceVulkan13Features& supported_features13,
    std::string& error) const {
    if (!dlss_requested_) return true;
    if (!required_feature_bits_supported(required_features12_,
                                         supported_features12)) {
        error = "Streamline requires unavailable Vulkan 1.2 feature bits";
        return false;
    }
    if (!required_feature_bits_supported(required_features13_,
                                         supported_features13)) {
        error = "Streamline requires unavailable Vulkan 1.3 feature bits";
        return false;
    }
    return true;
}

std::vector<const char*> StreamlineBridge::merge_extensions(
    const std::vector<const char*>& first, const std::vector<const char*>& second) {
    std::vector<const char*> merged;
    merged.reserve(first.size() + second.size());
    const auto append_unique = [&merged](const std::vector<const char*>& names) {
        for (const char* name : names) {
            const bool already_present = std::any_of(
                merged.begin(), merged.end(), [name](const char* existing) {
                    return std::strcmp(existing, name) == 0;
                });
            if (!already_present) merged.push_back(name);
        }
    };
    append_unique(first);
    append_unique(second);
    return merged;
}

void StreamlineBridge::append_requirements(
    std::vector<const char*>& instance_extensions,
    std::vector<const char*>& device_extensions,
    VkPhysicalDeviceVulkan12Features& features12,
    VkPhysicalDeviceVulkan13Features& features13, uint32_t& graphics_queue_count,
    uint32_t& compute_queue_count) const {
    instance_extensions = merge_extensions(instance_extensions, instance_extensions_);
    device_extensions = merge_extensions(device_extensions, device_extensions_);
    const bool has_khr_buffer_device_address = std::any_of(
        device_extensions.begin(), device_extensions.end(), [](const char* name) {
            return std::strcmp(name,
                               VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0;
        });
    if (has_khr_buffer_device_address) {
        device_extensions.erase(
            std::remove_if(device_extensions.begin(), device_extensions.end(),
                           [](const char* name) {
                               return std::strcmp(
                                          name,
                                          VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) ==
                                      0;
                           }),
            device_extensions.end());
    }
    merge_feature_bits(features12, required_features12_);
    merge_feature_bits(features13, required_features13_);
    // Streamline core creates a VkPrivateDataSlotEXT after device setup. The
    // 2.12 DLSS feature requirements do not consistently enumerate this core
    // Vulkan 1.3 bit, so make the actual runtime dependency explicit.
    if (dlss_requested_) features13.privateData = VK_TRUE;
    graphics_queue_count += additional_graphics_queues_;
    compute_queue_count += additional_compute_queues_;
}

bool StreamlineBridge::set_vulkan_info(
    VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
    uint32_t graphics_queue_family, uint32_t graphics_queue_index,
    uint32_t compute_queue_family, uint32_t compute_queue_index) {
#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
    if (!dlss_requested_) return true;
    sl::VulkanInfo info{};
    info.instance = instance;
    info.physicalDevice = physical_device;
    info.device = device;
    info.graphicsQueueFamily = graphics_queue_family;
    info.graphicsQueueIndex = graphics_queue_index;
    info.computeQueueFamily = compute_queue_family;
    info.computeQueueIndex = compute_queue_index;
    const sl::Result result = requires_explicit_vulkan_info(device_created_by_proxy_)
                                  ? reinterpret_cast<SlSetVulkanInfoFn>(
                                        sl_set_vulkan_info_)(info)
                                  : sl::Result::eOk;
    if (result != sl::Result::eOk) {
        disable(result_reason("slSetVulkanInfo", result));
        return true;
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical_device;
    const sl::Result support_result =
        reinterpret_cast<SlIsFeatureSupportedFn>(sl_is_feature_supported_)(
            sl::kFeatureDLSS, adapter);
    if (support_result != sl::Result::eOk) {
        disable(result_reason("slIsFeatureSupported(DLSS)", support_result));
        return true;
    }
    const auto get_feature_function = [this](const char* name,
                                             void*& function) {
        function = nullptr;
        return reinterpret_cast<SlGetFeatureFunctionFn>(
            sl_get_feature_function_)(sl::kFeatureDLSS, name, function);
    };
    sl::Result feature_result = get_feature_function(
        "slDLSSGetOptimalSettings", sl_dlss_get_optimal_settings_);
    if (feature_result == sl::Result::eOk)
        feature_result = get_feature_function("slDLSSSetOptions",
                                              sl_dlss_set_options_);
    if (feature_result != sl::Result::eOk ||
        !sl_dlss_get_optimal_settings_ || !sl_dlss_set_options_) {
        disable(result_reason("slGetFeatureFunction(DLSS)", feature_result));
        return true;
    }
    std::fprintf(stderr, "Streamline: DLSS support ready\n");
    dlss_available_ = true;
    return true;
#else
    (void)instance;
    (void)physical_device;
    (void)device;
    (void)graphics_queue_family;
    (void)graphics_queue_index;
    (void)compute_queue_family;
    (void)compute_queue_index;
    return true;
#endif
}

void StreamlineBridge::disable(std::string reason) {
    dlss_requested_ = false;
    dlss_available_ = false;
    dlss_unavailable_reason_ = std::move(reason);
    // A proxy-created Vulkan object must keep the interposer loaded and all
    // proxy calls routed until that object is destroyed.  The owner retries
    // the complete initialization natively after this stack is torn down.
    if (proxy_object_created_) return;
    use_proxy_dispatch_ = false;
    instance_extensions_.clear();
    device_extensions_.clear();
    required_features12_ = {};
    required_features13_ = {};
    additional_graphics_queues_ = 0;
    additional_compute_queues_ = 0;
#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
    if (streamline_initialized_ && sl_shutdown_) {
        reinterpret_cast<SlShutdownFn>(sl_shutdown_)();
        streamline_initialized_ = false;
    }
    if (streamline_module_) {
        FreeLibrary(static_cast<HMODULE>(streamline_module_));
        streamline_module_ = nullptr;
    }
#endif
}

void StreamlineBridge::shutdown() {
#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
    if (streamline_initialized_) {
        reinterpret_cast<SlShutdownFn>(sl_shutdown_)();
        streamline_initialized_ = false;
    }
    if (streamline_module_) {
        FreeLibrary(static_cast<HMODULE>(streamline_module_));
        streamline_module_ = nullptr;
    }
#endif
}

bool StreamlineBridge::populate_device_proxies(VkDevice device) {
    if (!get_device_proc_addr_proxy_) return false;
    queue_present_proxy_ = reinterpret_cast<PFN_vkQueuePresentKHR>(
        get_device_proc_addr_proxy_(device, "vkQueuePresentKHR"));
    create_swapchain_proxy_ = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        get_device_proc_addr_proxy_(device, "vkCreateSwapchainKHR"));
    get_swapchain_images_proxy_ = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
        get_device_proc_addr_proxy_(device, "vkGetSwapchainImagesKHR"));
    destroy_swapchain_proxy_ = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
        get_device_proc_addr_proxy_(device, "vkDestroySwapchainKHR"));
    acquire_next_image_proxy_ = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
        get_device_proc_addr_proxy_(device, "vkAcquireNextImageKHR"));
    device_wait_idle_proxy_ = reinterpret_cast<PFN_vkDeviceWaitIdle>(
        get_device_proc_addr_proxy_(device, "vkDeviceWaitIdle"));
    return queue_present_proxy_ && create_swapchain_proxy_ &&
           get_swapchain_images_proxy_ && destroy_swapchain_proxy_ &&
           acquire_next_image_proxy_ && device_wait_idle_proxy_;
}

VkResult StreamlineBridge::create_instance(
    const VkInstanceCreateInfo* create, const VkAllocationCallbacks* allocator,
    VkInstance* instance) {
    if (use_proxy_dispatch_) {
        const VkResult result = vkCreateInstance(create, allocator, instance);
        if (result == VK_SUCCESS) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
            if (test_proxy_fault_ == TestProxyFault::Instance) {
                disable("injected missing Streamline instance proxy");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            if (test_proxy_fault_ == TestProxyFault::Device) return result;
#endif
#ifdef _WIN32
            if (!get_instance_proc_addr_proxy_) {
                disable("Streamline SDK is missing the instance manual-hook dispatcher");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            create_win32_surface_proxy_ =
                reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
                    get_instance_proc_addr_proxy_(*instance,
                                                  "vkCreateWin32SurfaceKHR"));
            destroy_surface_proxy_ = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
                get_instance_proc_addr_proxy_(*instance, "vkDestroySurfaceKHR"));
            if (!create_win32_surface_proxy_ || !destroy_surface_proxy_) {
                disable("Streamline SDK could not provide required instance manual hooks");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
#endif
        }
        return result;
    }
    return vkCreateInstance(create, allocator, instance);
}

VkResult StreamlineBridge::create_device(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create,
    const VkAllocationCallbacks* allocator, VkDevice* device) {
    if (use_proxy_dispatch_) {
        const VkResult result =
            vkCreateDevice(physical_device, create, allocator, device);
        if (result == VK_SUCCESS) {
            device_created_by_proxy_ = false;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
            if (test_proxy_fault_ == TestProxyFault::Device) {
                disable("injected missing Streamline device proxy");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
#endif
            if (!populate_device_proxies(*device)) {
                disable("Streamline SDK could not provide required Vulkan manual hooks");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        return result;
    }
    return vkCreateDevice(physical_device, create, allocator, device);
}

VkResult StreamlineBridge::get_swapchain_images(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t* image_count,
    VkImage* images) {
    if (use_proxy_dispatch_ && get_swapchain_images_proxy_) {
        proxy_dispatch_used_ = true;
        return get_swapchain_images_proxy_(device, swapchain, image_count, images);
    }
    return vkGetSwapchainImagesKHR(device, swapchain, image_count, images);
}

VkResult StreamlineBridge::queue_present(VkQueue queue,
                                         const VkPresentInfoKHR* present) {
    if (use_proxy_dispatch_ &&
        (!present_common_pending_ || present_common_serial_ == 0)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (use_proxy_dispatch_) {
        present_common_pending_ = false;
        present_common_serial_ = 0;
    }
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    record_test_presentation_event("present");
#endif
    if (use_proxy_dispatch_ && queue_present_proxy_) {
        proxy_dispatch_used_ = true;
        return queue_present_proxy_(queue, present);
    }
    return vkQueuePresentKHR(queue, present);
}

bool StreamlineBridge::present_common(uint64_t frame_serial) {
    if (frame_serial == 0) return false;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    last_present_common_serial_ = frame_serial;
    record_test_presentation_event("present_common");
#endif
    if (!use_proxy_dispatch_) return true;
    if (present_common_pending_) return false;
    // vkQueuePresentKHR is one of Streamline's required Vulkan hooks.  Its
    // proxy enters the common plugin's presentCommon() implementation.
    present_common_pending_ = true;
    present_common_serial_ = frame_serial;
    return true;
}

VkResult StreamlineBridge::create_swapchain(
    VkDevice device, const VkSwapchainCreateInfoKHR* create,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    record_test_presentation_event("create_swapchain");
#endif
    if (use_proxy_dispatch_ && create_swapchain_proxy_) {
        proxy_dispatch_used_ = true;
        const VkResult result =
            create_swapchain_proxy_(device, create, allocator, swapchain);
        if (result == VK_SUCCESS) proxy_object_created_ = true;
        return result;
    }
    return vkCreateSwapchainKHR(device, create, allocator, swapchain);
}

void StreamlineBridge::destroy_swapchain(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* allocator) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    record_test_presentation_event("destroy_swapchain");
#endif
    if (use_proxy_dispatch_ && destroy_swapchain_proxy_) {
        proxy_dispatch_used_ = true;
        destroy_swapchain_proxy_(device, swapchain, allocator);
        return;
    }
    vkDestroySwapchainKHR(device, swapchain, allocator);
}

VkResult StreamlineBridge::acquire_next_image(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t* image_index) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    record_test_presentation_event("acquire");
#endif
    if (use_proxy_dispatch_ && acquire_next_image_proxy_) {
        proxy_dispatch_used_ = true;
        return acquire_next_image_proxy_(device, swapchain, timeout, semaphore,
                                         fence, image_index);
    }
    return vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence,
                                 image_index);
}

VkResult StreamlineBridge::device_wait_idle(VkDevice device) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    record_test_presentation_event("device_wait_idle");
#endif
    if (use_proxy_dispatch_ && device_wait_idle_proxy_) {
        proxy_dispatch_used_ = true;
        return device_wait_idle_proxy_(device);
    }
    return vkDeviceWaitIdle(device);
}

#ifdef _WIN32
VkResult StreamlineBridge::create_win32_surface(
    VkInstance instance, const VkWin32SurfaceCreateInfoKHR* create,
    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    if (use_proxy_dispatch_ && create_win32_surface_proxy_) {
        proxy_dispatch_used_ = true;
        const VkResult result =
            create_win32_surface_proxy_(instance, create, allocator, surface);
        if (result == VK_SUCCESS) proxy_object_created_ = true;
        return result;
    }
    return vkCreateWin32SurfaceKHR(instance, create, allocator, surface);
}
#endif

void StreamlineBridge::destroy_surface(
    VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks* allocator) {
    if (use_proxy_dispatch_ && destroy_surface_proxy_) {
        proxy_dispatch_used_ = true;
        destroy_surface_proxy_(instance, surface, allocator);
        return;
    }
    vkDestroySurfaceKHR(instance, surface, allocator);
}

}  // namespace matter
