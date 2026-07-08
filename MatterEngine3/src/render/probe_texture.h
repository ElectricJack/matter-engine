#pragma once
// Direct-GL 3D-texture shim: rlgl has no 3D-texture API. Uses raylib's bundled
// glad loader (function pointers are live after InitWindow on both Linux and the
// MinGW Windows build). Requires a current GL context — viewer-side only.
#include "probe_volume.h"
#include <cstdint>

namespace viewer {

// Quantization scale for ambient rgb and dominant intensity into RGBA8.
constexpr float kProbeAmbientScale = 4.0f;

struct ProbeTextures {
    unsigned int tex_ambient  = 0;   // GL texture ids (GL_TEXTURE_3D)
    unsigned int tex_dominant = 0;
    probe_volume::ProbeGrid grid;
    bool valid() const { return tex_ambient != 0 && tex_dominant != 0; }
};

// Uploads both volumes as RGBA8 3D textures, GL_LINEAR min/mag, CLAMP_TO_EDGE.
// A.rgb = clamp(ambient/kProbeAmbientScale) ; A.a = sun_vis
// B.rgb = dir*0.5+0.5                       ; B.a = clamp(intensity/kProbeAmbientScale)
ProbeTextures upload_probe_textures(const probe_volume::ProbeVolume& v);
void release_probe_textures(ProbeTextures& t);

} // namespace viewer
