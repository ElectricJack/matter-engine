#pragma once
#include <matter/contracts/graphics.h>
#include <matter/contracts/hosts.h>
#include <matter/contracts/terrain.h>

// Contract fixtures only. These factories implement no production algorithms.
std::unique_ptr<matter::services::GeometryService> fake_geometry();
std::unique_ptr<matter::services::ResolverService> fake_resolver();
std::unique_ptr<matter::services::AuthoringHost> fake_host();
std::unique_ptr<matter::services::CacheService> fake_cache();
std::unique_ptr<matter::services::GraphicsService> fake_graphics();
std::unique_ptr<matter::services::TerrainService> fake_terrain();
std::unique_ptr<matter::services::BuildService> fake_build(
    matter::services::ResolverService &, matter::services::AuthoringHost &,
    matter::services::GeometryService &);
