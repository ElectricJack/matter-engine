#pragma once
#include <matter/contracts/jobs.h>

namespace matter::compute {
namespace data = matter::contracts;
namespace jobs = matter::services;
// Process-local service identity, not a portable asset identifier. No handles
// escape execute(): all returned bytes are independently owned CPU storage.
using DeviceId = uint64_t;
struct Buffer {
    uint64_t size = 0;
    std::vector<uint8_t> initial;
    bool readback = false;
};
struct Dispatch {
    // Trusted, compiled backend SPIR-V; this is not an untrusted shader sandbox.
    std::vector<uint32_t> spirv;
    uint32_t groups_x = 1;
};
struct Request {
    DeviceId device = 0;
    std::vector<Buffer> buffers; // storage bindings 0..N-1
    std::vector<Dispatch> dispatches; // ordered, full compute barriers between
    uint64_t allocation_limit = 64ull * 1024 * 1024;
};
struct Readback {
    // One entry per binding; empty for non-readback bindings.
    std::vector<std::vector<uint8_t>> buffers;
    uint64_t allocated_bytes = 0;
};
using Result = jobs::JobResult<Readback>;
class Device {
public:
    Device();
    virtual ~Device() = default;
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;
    DeviceId id() const noexcept { return id_; }
    // Synchronous, serialized by the implementation. Inputs live through return.
    // Cancellation/deadline are cooperative; a submitted dispatch must finish
    // before storage is released. Failure/cancellation returns no readback.
    virtual Result execute(const jobs::JobEnvelope &, const Request &,
        jobs::JobMonitor &) = 0;
private:
    DeviceId id_;
};
} // namespace matter::compute
