#pragma once
#include <cstring>
#include <matter/asset/archive.h>
#include <stdexcept>

namespace matter::asset::detail {
using E = data::ErrorCode;
struct Failure : std::runtime_error {
  E code;
  Failure(E c, const std::string &s) : std::runtime_error(s), code(c) {}
};
inline void need(bool b, const std::string &s, E c = E::invalid_input) {
  if (!b)
    throw Failure(c, s);
}
inline void checked(const data::ValidationResult &r) {
  if (!r.ok())
    throw Failure(r.diagnostics.front().code, r.diagnostics.front().message);
}
inline std::string_view view(const Bytes &b) {
  return {reinterpret_cast<const char *>(b.data()), b.size()};
}
inline void u32(Bytes &b, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i)
    b.push_back(uint8_t(v >> (8 * i)));
}
inline uint32_t read32(const Bytes &b, size_t at) {
  need(at <= b.size() && b.size() - at >= 4, "truncated uint32");
  uint32_t n = 0;
  for (unsigned i = 0; i < 4; ++i)
    n |= uint32_t(b[at + i]) << (8 * i);
  return n;
}
inline void size32(Bytes &b, size_t v) {
  need(v <= UINT32_MAX, "format length overflow", E::resource_limit);
  u32(b, static_cast<uint32_t>(v));
}
inline void scalar(Bytes &b, float v) {
  uint32_t n;
  std::memcpy(&n, &v, 4);
  u32(b, n);
}
inline float scalar(const Bytes &b, size_t at) {
  auto n = read32(b, at);
  float v;
  std::memcpy(&v, &n, 4);
  return v;
}
inline bool digest(const std::string &s) {
  return s.size() == 64 &&
         s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
template <class Result, class F>
Result boundary(const services::JobEnvelope &job, F &&fn) {
  try {
    checked(services::validate(job));
    return fn();
  } catch (const Failure &e) {
    return Result::failure(job, e.code, e.what());
  } catch (const std::bad_alloc &) {
    return Result::failure(job, E::resource_limit, "allocation failed");
  } catch (const std::exception &e) {
    return Result::failure(job, E::backend_failure, e.what());
  } catch (...) {
    return Result::failure(job, E::backend_failure,
                           "unknown adapter exception");
  }
}
} // namespace matter::asset::detail
