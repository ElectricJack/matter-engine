#pragma once

namespace viewer {
// Healthy cache: one load. Cold generation: one load, no retries. Only an
// eligible failed cached load gets one regeneration followed by one last load.
template<class Generate, class Load, class CanRecover>
bool load_detail_with_cache_recovery(bool cached, Generate generate, Load load,
                                    CanRecover can_recover, bool& rebuilt) {
    rebuilt = false;
    if (!cached && !generate()) return false;
    if (load()) return true;
    if (!cached || !can_recover()) return false;
    rebuilt = true;
    return generate() && load();
}
} // namespace viewer
