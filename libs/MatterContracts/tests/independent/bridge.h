#pragma once
#include <matter/contracts/terrain.h>

// Test-only callee declarations. No implementation/internal headers cross here.
namespace gate {
matter::contracts::RecipeDocument recipe();
matter::contracts::AssetDocument asset();
matter::services::AssetResult complete(const matter::services::JobEnvelope &,
                                      const matter::services::EvaluationOptions &,
                                      const matter::services::JobControl &);
matter::services::TerrainProduct terrain(const matter::services::TerrainRequest &);
}
