bool VkSceneRenderer::ensure_dlss_output(FrameResources& frame,
                                         VkExtent2D output_extent,
                                         std::string& error) {
    if (output_extent.width == 0 || output_extent.height == 0) {
        error = "DLSS output extent must be nonzero";
        return false;
    }
    if (frame.dlss_output.image != VK_NULL_HANDLE &&
        frame.dlss_output_extent.width == output_extent.width &&
        frame.dlss_output_extent.height == output_extent.height) {
        return true;
    }
    matter::VkImageResource replacement;
    if (!matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT,
            {output_extent.width, output_extent.height, 1},
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            replacement, error)) {
        return false;
    }
    frame.dlss_output = std::move(replacement);
    frame.dlss_output_extent = output_extent;
    return true;
}
