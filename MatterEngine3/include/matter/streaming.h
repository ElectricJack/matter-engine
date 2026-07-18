#pragma once
#include <cstdint>
#include "flecs.h"

namespace matter::streaming {
struct SectorStreaming {};
enum class SectorStreamingErrorCode : uint8_t {
    None, UnsupportedWorld, OwnerAlreadyClaimed
};
struct SectorStreamingError {
    SectorStreamingErrorCode code = SectorStreamingErrorCode::None;
    flecs::entity_t active_owner = 0;
};
enum class SectorStreamingState : uint8_t {
    Detached, PendingProfile, PendingTransform, Active, Detaching
};
struct SectorStreamingStatus {
    SectorStreamingState state = SectorStreamingState::Detached;
    uint64_t generation = 0;
    uint32_t resident_sectors = 0;
    uint32_t inflight_sectors = 0;
};
struct StreamingUpdate {};
struct StreamingModule { explicit StreamingModule(flecs::world& world); };
} // namespace matter::streaming
