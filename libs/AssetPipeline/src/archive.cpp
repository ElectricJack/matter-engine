#include "internal.h"
#include <algorithm>

namespace matter::asset {
using namespace detail;
BytesResult encode_prepared(const services::JobEnvelope &job,
                            const services::AssetProduct &p,
                            const services::EvaluationOptions &o,
                            const services::JobControl &c) {
  return boundary<BytesResult>(job, [&] {
    services::JobMonitor monitor(o.budget, c);
    checked(monitor.checkpoint());
    checked(services::validate(p, o.validation));
    need(p.resources.assets.empty(),
         "prepared product cannot contain dependency assets",
         E::unsupported_capability);
    need(p.resources.resources.size() == p.document.resources.size(),
         "prepared product must contain exactly its declared resources");
    services::ResourceUsage usage;
    for (const auto &mesh : p.document.meshes) {
      need(mesh.positions.size() <= o.budget.vertices - usage.vertices &&
               mesh.indices.size() <= o.budget.indices - usage.indices,
           "prepared mesh exceeds job budget", E::resource_limit);
      usage.vertices += mesh.positions.size();
      usage.indices += mesh.indices.size();
    }
    checked(monitor.checkpoint(usage));
    std::string json;
    checked(data::encode(p.document, json, o.validation));
    Bytes body;
    need(json.size() <= o.budget.output_bytes &&
             json.size() <= o.budget.working_bytes,
         "prepared document exceeds budget", E::resource_limit);
    size32(body, json.size());
    body.insert(body.end(), json.begin(), json.end());
    size32(body, p.document.resources.size());
    for (const auto &r : p.document.resources) {
      checked(monitor.checkpoint());
      auto it = std::find_if(
          p.resources.resources.begin(), p.resources.resources.end(),
          [&](const auto &a) { return a.description.id == r.id; });
      need(it != p.resources.resources.end() && bool(it->bytes),
           "prepared resource missing", E::missing_dependency);
      const auto &bytes = *it->bytes;
      need(body.size() <= o.budget.output_bytes &&
               bytes.size() <= o.budget.output_bytes - body.size() &&
               body.size() + bytes.size() + 84 <= o.budget.output_bytes &&
               body.size() + bytes.size() + 84 <= o.budget.working_bytes,
           "prepared payload exceeds budget", E::resource_limit);
      size32(body, bytes.size());
      body.insert(body.end(), bytes.begin(), bytes.end());
    }
    Bytes out{'M', 'A', 'P', 'A'};
    u32(out, 1);
    size32(out, body.size());
    auto hash = data::sha256(view(body));
    out.insert(out.end(), hash.hex.begin(), hash.hex.end());
    out.insert(out.end(), body.begin(), body.end());
    usage.output_bytes = out.size();
    usage.working_bytes = out.size();
    checked(monitor.checkpoint(usage));
    return BytesResult::success(job, std::move(out));
  });
}
services::AssetResult decode_prepared(const services::JobEnvelope &job,
                                      const Bytes &b,
                                      const services::EvaluationOptions &o,
                                      const services::JobControl &c) {
  return boundary<services::AssetResult>(job, [&] {
    services::JobMonitor monitor(o.budget, c);
    checked(monitor.checkpoint());
    need(b.size() <= o.budget.working_bytes, "prepared input exceeds budget",
         E::resource_limit);
    need(b.size() >= 84 && view(b).substr(0, 4) == "MAPA",
         "invalid prepared archive magic");
    need(read32(b, 4) == 1, "unsupported prepared archive version",
         E::unsupported_version);
    need(read32(b, 8) == b.size() - 76, "prepared archive length mismatch");
    auto body = view(b).substr(76);
    need(data::sha256(body).hex == view(b).substr(12, 64),
         "prepared archive SHA-256 mismatch");
    size_t at = 76;
    const auto length = read32(b, at);
    at += 4;
    need(length <= b.size() - at, "truncated prepared document");
    data::AssetDocument doc;
    checked(data::decode_asset(view(b).substr(at, length), doc, o.validation));
    at += length;
    const auto count = read32(b, at);
    at += 4;
    need(count == doc.resources.size(), "prepared resource count mismatch");
    services::ResolvedInputs resources;
    for (const auto &r : doc.resources) {
      checked(monitor.checkpoint());
      auto n = read32(b, at);
      at += 4;
      need(n == r.byte_length && n <= b.size() - at,
           "prepared payload length mismatch");
      auto bytes = std::make_shared<const Bytes>(
          b.begin() + static_cast<ptrdiff_t>(at),
          b.begin() + static_cast<ptrdiff_t>(at + n));
      resources.resources.push_back({r, std::move(bytes)});
      at += n;
    }
    need(at == b.size(), "trailing prepared bytes");
    services::ResourceUsage usage;
    usage.working_bytes = b.size();
    return services::finish_asset(job, std::move(doc), o, monitor, usage,
                                  std::move(resources));
  });
}
} // namespace matter::asset
