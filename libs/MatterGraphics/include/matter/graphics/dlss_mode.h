#pragma once

#include <cstdint>

namespace matter {

// Requested DLSS preset (RenderOptions::dlss_mode). Native renders at output
// resolution with no upscaling; the other three are Streamline presets at
// successively smaller internal render resolutions. This is a REQUEST: the mode
// that actually ran, the internal/output extents and the reason for any
// downgrade come back in FrameStats::dlss_*. dlss_mode_name() below gives the
// display string.
enum class DlssMode : uint8_t { Native, Quality, Balanced, Performance };

const char* dlss_mode_name(DlssMode mode) noexcept;

}  // namespace matter
