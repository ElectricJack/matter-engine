#pragma once

// Keep the module's subset registry distinct from the host's full registry.
#ifdef MATTER_GRAPHICS_NATIVE_ENVIRONMENT
#include "environment_embedded_spirv.h"
namespace viewer { namespace environment_spirv = matter::graphics::environment_spirv; }
#else
#include "shaders_gen/embedded_spirv.h"
namespace viewer { namespace environment_spirv = matter; }
#endif
