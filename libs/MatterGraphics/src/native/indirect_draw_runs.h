#pragma once
#include <algorithm>
#include <cstdint>

namespace viewer {
// Caller has already bound identical pipeline/descriptors/vertex/index state
// for every range. Coalesce only touching accepted spans in their input order;
// never sort draws or include water. The first part slot is a debug hint only.
template<class Range, class Emit>
uint32_t for_each_opaque_indirect_run(const Range* ranges, uint32_t count,
                                    uint32_t command_count, uint32_t device_limit,
                                    Emit emit) {
    const uint32_t limit = std::max(1u, device_limit);
    Range pending{};
    uint32_t issued = 0;
    const auto flush = [&] {
        while (pending.command_count) {
            auto chunk = pending;
            chunk.command_count = std::min(pending.command_count, limit);
            emit(chunk); ++issued;
            pending.first_command += chunk.command_count;
            pending.command_count -= chunk.command_count;
        }
    };
    for (uint32_t i=0; i<count; ++i) {
        const auto& range = ranges[i];
        if (range.raster_water_surface || !range.command_count ||
            range.first_command > command_count ||
            range.command_count > command_count - range.first_command) continue;
        if (pending.command_count &&
            pending.first_command + pending.command_count == range.first_command) {
            // Both ranges are bounded by command_count, so their contiguous
            // union also fits uint32_t. Splitting to the device limit is later.
            pending.command_count += range.command_count;
        } else {
            flush(); pending = range;
        }
    }
    flush(); return issued;
}
} // namespace viewer
