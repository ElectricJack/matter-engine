#include <matter/compute/device.h>
#include <atomic>
namespace matter::compute {
namespace { std::atomic<uint64_t> next{1}; }
Device::Device() : id_(next.fetch_add(1, std::memory_order_relaxed)) {}
}
