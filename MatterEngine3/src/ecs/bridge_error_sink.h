// bridge_error_sink.h — the BridgeErrorSink notification contract.
//
// Extracted from dynamic_scene_bridge.h (I.11) so the sink type is reachable
// without pulling in flecs / the render slot table: the hub-backed adapter
// (bridge_error_hub.h) and its focused test only need this struct + scene
// types. dynamic_scene_bridge.h includes this header, so every existing user
// of the type is unaffected.
#pragma once

#include "matter/scene.h"

#include <functional>

namespace matter::scene {

// Callback interface for the DynamicSceneBridge to report per-entity errors.
// The bridge does NOT mutate the ECS world directly — it reports errors
// through these callbacks so the caller can apply them safely (e.g. by
// setting a PartInstanceError component).
//
// Either callback may be left EMPTY: every call site in the bridge tests the
// std::function before invoking it, so a caller that only cares about one half
// can supply one half. Both are invoked synchronously, inline in
// DynamicSceneBridge::reconcile, on whichever thread runs the reconcile.
//
// Delivery cadence is not symmetric. on_error fires on every reconcile for as
// long as the condition persists (a capacity failure re-reports each frame),
// while on_error_clear fires once, on the transition back to healthy — the
// bridge remembers per entity whether it last reported an error and only calls
// the clear on the edge.
struct BridgeErrorSink {
    std::function<void(SceneEntityId id, PartInstanceError error)> on_error;
    std::function<void(SceneEntityId id)> on_error_clear;
};

}  // namespace matter::scene
