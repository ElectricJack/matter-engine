#pragma once
#include <map>
#include <matter/asset/archive.h>

namespace matter::asset {
struct GltfDocument {
  std::string json;
  // Resolved ordered buffers. External resolution belongs to the caller.
  std::vector<Bytes> buffers;
  std::map<std::string, Bytes> images; // Exact declared URI -> resolved bytes.
};
using GltfResult = services::JobResult<GltfDocument>;
std::vector<data::Capability> gltf_capabilities();
GltfResult export_gltf(const services::JobEnvelope &,
                       const services::AssetProduct &,
                       const services::EvaluationOptions & = {},
                       const services::JobControl & = {});
BytesResult export_glb(const services::JobEnvelope &,
                       const services::AssetProduct &,
                       const services::EvaluationOptions & = {},
                       const services::JobControl & = {});
services::AssetResult import_gltf(const services::JobEnvelope &,
                                  const GltfDocument &,
                                  const services::EvaluationOptions & = {},
                                  const services::JobControl & = {});
services::AssetResult import_glb(const services::JobEnvelope &, const Bytes &,
                                 const services::EvaluationOptions & = {},
                                 const services::JobControl & = {});
} // namespace matter::asset
