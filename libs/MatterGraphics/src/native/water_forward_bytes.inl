uint64_t water_forward_image_bytes_for_extent(uint32_t width,
                                              uint32_t height) noexcept {
    constexpr uint64_t kBytesPerPixel = 8u + 4u;
    const uint64_t pixels = static_cast<uint64_t>(width) *
                            static_cast<uint64_t>(height);
    if (pixels == 0u ||
        pixels > std::numeric_limits<uint64_t>::max() / kBytesPerPixel)
        return 0u;
    return pixels * kBytesPerPixel;
}
