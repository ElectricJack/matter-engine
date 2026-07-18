#pragma once

#include "flecs.h"

namespace matter::streaming::detail {

class Coordinator;
struct Snapshot;

struct StreamingContextRef {
    Coordinator* value = nullptr;
};

void register_streaming_systems(flecs::world& world);
void publish_streaming_snapshot(
    flecs::world& world,
    const Snapshot& snapshot);

} // namespace matter::streaming::detail
