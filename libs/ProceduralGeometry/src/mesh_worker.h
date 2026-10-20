#pragma once
#include "mesh_worker_protocol.h"
#include <matter/contracts/generation.h>

namespace matter::geometry::detail {
bool mesh_worker_available(const std::string &path);
MeshIndexed run_mesh_worker(const std::string &path,const MeshIndexed &,const wire::Options &,
    const services::ResourceBudget &,services::JobMonitor &,services::ResourceUsage &);
}
