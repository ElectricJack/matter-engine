#pragma once
#include <cmath>
#include <limits>
#include <matter/contracts/contracts.h>
#include <set>
#include <stdexcept>

namespace matter::contracts::detail {
inline Diagnostic error(ErrorCode code, std::string message,
                        std::string node = {}) {
  Diagnostic d;
  d.code = code;
  d.message = std::move(message);
  d.node_id = std::move(node);
  return d;
}
inline ValidationResult failure(ErrorCode code, std::string message) {
  return {{error(code, std::move(message))}};
}
bool valid_utf8(std::string_view text);
inline bool valid_id(const SemanticId &id) {
  if (id.value.empty() || id.value.size() > 256)
    return false;
  auto alnum = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
  };
  if (!alnum(static_cast<unsigned char>(id.value[0])))
    return false;
  for (unsigned char c : id.value)
    if (!alnum(c) && c != '.' && c != '_' && c != ':' && c != '/' && c != '-')
      return false;
  return true;
}
inline bool valid_digest(const ContentDigest &d) {
  return d.hex.size() == 64 &&
         d.hex.find_first_not_of("0123456789abcdef") == std::string::npos;
}
inline bool finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
inline bool finite(Vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
inline bool finite(Vec4 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
         std::isfinite(v.w);
}
inline bool finite(Quat v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
         std::isfinite(v.w);
}
std::string hex64(uint64_t n);
} // namespace matter::contracts::detail
