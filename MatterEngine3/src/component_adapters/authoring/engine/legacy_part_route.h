#pragma once
// Engine-side adapter: hands part sources that declare
//   static authoring = "matter.authoring.portable.v1";
// from part_graph::HostBaker to the portable authoring pipeline
// (QuickJS host -> Coordinator -> geometry) and publishes the result as an
// ordinary .part. The kernel only sees part_graph::PortableBakeRoute.
#include "part_graph.h"
#include <memory>

#if defined(MATTER_HAVE_SCRIPT_HOST)
namespace matter::authoring_adapter {
std::shared_ptr<part_graph::PortableBakeRoute> make_legacy_part_route();
} // namespace matter::authoring_adapter
#endif
