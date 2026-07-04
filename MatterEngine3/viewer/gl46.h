#ifndef VIEWER_GL46_H
#define VIEWER_GL46_H
#include "external/glad.h"   // raylib's bundled full glad; loaded by rlLoadExtensions
#include <cstdlib>
#include <string>

namespace viewer {

// True when the live context exposes everything the GPU cull path needs.
// Call AFTER InitWindow (glad must be loaded).
inline bool gl46_available(std::string& why) {
    GLint maj = 0, min = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &maj);
    glGetIntegerv(GL_MINOR_VERSION, &min);
    if (maj * 10 + min < 43) {   // compute/SSBO/indirect floor; 4.6 gives gl_BaseInstance in GLSL
        why = "GL " + std::to_string(maj) + "." + std::to_string(min) + " < 4.3";
        return false;
    }
    if (!glDispatchCompute)           { why = "glDispatchCompute missing";           return false; }
    if (!glMultiDrawArraysIndirect)   { why = "glMultiDrawArraysIndirect missing";   return false; }
    if (!glBindBufferBase)            { why = "glBindBufferBase missing";            return false; }
    if (!glMemoryBarrier)             { why = "glMemoryBarrier missing";             return false; }
    if (maj * 10 + min < 46) {
        // gl_BaseInstance needs GLSL 460; SPIR-V not used. Hard-require 4.6 per spec.
        why = "GL " + std::to_string(maj) + "." + std::to_string(min) + " < 4.6 (gl_BaseInstance)";
        return false;
    }
    return true;
}

inline bool gpu_cull_requested() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("MATTER_GPU_CULL"); v = (e && e[0] == '1') ? 1 : 0; }
    return v == 1;
}

} // namespace viewer
#endif
