// tileset_bake_gpu_stub.cpp — stub for headless test builds (no GL available).
// The bake_tileset_gpu function is only called via the 6-arg overload of
// run_tileset_phase, which tests don't use. This stub satisfies the linker.

#include "../include/tileset_bake_gpu.h"
#include <string>

namespace tileset {

bool bake_tileset_gpu(const SettledTorus&,
                      uint64_t,
                      const std::string&,
                      const BakeInputs&,
                      bool,
                      bool,
                      std::string& err)
{
    err = "bake_tileset_gpu stub: GPU bake not available in headless test build";
    return false;
}

} // namespace tileset
