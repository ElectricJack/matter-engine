#pragma once
#include <matter/geometry/geometry.h>
#include <matter/geometry_c.h>
#include <stdexcept>

namespace matter::binding {
namespace d = contracts;
namespace s = services;
using V = d::Value;
using O = V::Object;
using A = V::Array;
struct Failure { matter_status status; const char *message; };
[[noreturn]] inline void fail(matter_status status, const char *message) { throw Failure{status,message}; }
inline void require(bool yes, const char *message) { if (!yes) fail(MATTER_INVALID_INPUT,message); }
matter_status status(d::ErrorCode);
void checked(const d::ValidationResult &);
const O &object(const V &);
const A &array(const V &);
const V &field(const V &, const char *);
std::string string(const V &);
double number(const V &);
uint32_t u32(const V &);
void keys(const V &, std::initializer_list<const char *>);
std::vector<d::Capability> capabilities(const V &);
V capabilities(const std::vector<d::Capability> &);
std::string hex64(uint64_t);
V envelope(const s::JobEnvelope &);
std::string json(const V &, const d::ValidationLimits &);
s::ResolvedInputs resources(const V &, const std::vector<d::ResourceDescription> &, uint64_t);
V resources(const s::ResolvedInputs &);
s::QueryBatch query(const V &);
V samples(const s::QueryProduct &);
V diagnostics(const std::vector<d::Diagnostic> &);
// Conservative admission for the temporary JSON value tree around an asset.
uint64_t serialization_work(const s::AssetProduct &);
} // namespace matter::binding
