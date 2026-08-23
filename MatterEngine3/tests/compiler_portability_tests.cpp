#include "matter/compiler.h"
#include "matter/event/event_hub.h"
#include "matter/event/event_name.h"
#include "bvh.h"
#include "check.h"

#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace {

struct MATTER_ALIGN(32) AlignedProbe {
    unsigned char value = 0;
};

struct CallsiteEvent {
    MT_EVENT_NAME("test.compiler.callsite");
};

#if defined(_MSC_VER)
#define MATTER_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define MATTER_TEST_NOINLINE __attribute__((noinline))
#else
#define MATTER_TEST_NOINLINE
#endif

struct ReturnAddressProbe {
    void* expected;
    void* actual;
};

MATTER_TEST_NOINLINE ReturnAddressProbe capture_return_address() {
#if defined(_MSC_VER)
    void* expected = _ReturnAddress();
#elif defined(__GNUC__) || defined(__clang__)
    void* expected = __builtin_extract_return_addr(__builtin_return_address(0));
#else
    void* expected = nullptr;
#endif
    return ReturnAddressProbe{expected, matter::diagnostics::return_address()};
}

void format_probe(const char*, ...) MATTER_PRINTF_FORMAT(1, 2);

void format_probe(const char*, ...) {}

static_assert(alignof(AlignedProbe) == 32, "MATTER_ALIGN must preserve requested alignment");
static_assert(alignof(float2) == 8, "float2 public alignment changed");
static_assert(alignof(uint2) == 8, "uint2 public alignment changed");
static_assert(alignof(int2) == 8, "int2 public alignment changed");
static_assert(alignof(float4) == 16, "float4 public alignment changed");
static_assert(alignof(Tri) == 64, "Tri public alignment changed");
static_assert(alignof(BVHRay) == 64, "BVHRay public alignment changed");
static_assert(alignof(BVH) == 64, "BVH public alignment changed");
static_assert(alignof(TLAS) == 64, "TLAS public alignment changed");

static_assert(std::is_standard_layout<Tri>::value, "serialized Tri must remain standard-layout");
static_assert(std::is_standard_layout<TriEx>::value, "serialized TriEx must remain standard-layout");
static_assert(std::is_standard_layout<BVHNode>::value,
              "serialized BVHNode must remain standard-layout");
static_assert(sizeof(Tri) == 64, "serialized Tri size changed");
static_assert(sizeof(TriEx) == 96, "serialized TriEx size changed");
static_assert(sizeof(BVHNode) == 32, "serialized BVHNode size changed");
static_assert(offsetof(BVHNode, aabbMin) == 0, "serialized BVHNode aabbMin moved");
static_assert(offsetof(BVHNode, leftFirst) == 12, "serialized BVHNode leftFirst moved");
static_assert(offsetof(BVHNode, aabbMax) == 16, "serialized BVHNode aabbMax moved");
static_assert(offsetof(BVHNode, triCount) == 28, "serialized BVHNode triCount moved");
static_assert(offsetof(Tri, vertex0) == 0, "serialized Tri vertex0 moved");
static_assert(offsetof(Tri, vertex1) == 16, "serialized Tri vertex1 moved");
static_assert(offsetof(Tri, vertex2) == 32, "serialized Tri vertex2 moved");
static_assert(offsetof(Tri, centroid) == 48, "serialized Tri centroid moved");
static_assert(offsetof(TriEx, ao2) == 88, "serialized TriEx ao2 moved");

void test_callsite_capture() {
    matter::evt::Hub hub;
    const int expected_line = __LINE__ + 1;
    auto subscription = hub.must_subscribe<CallsiteEvent>(
        "compiler.callsite", matter::evt::immediate, [](const CallsiteEvent&) {});

    const auto snapshot = hub.registry_snapshot();
    CHECK(snapshot.size() == 1, "callsite probe registers one event type");
    if (snapshot.size() != 1) return;
    CHECK(snapshot.front().subscribers.size() == 1,
          "callsite probe registers exactly one subscriber");
    if (snapshot.front().subscribers.size() != 1) return;

    const auto& subscriber = snapshot.front().subscribers.front();
    CHECK(subscriber.file != nullptr && std::strcmp(subscriber.file, __FILE__) == 0,
          "subscription captures the caller file");
    CHECK(subscriber.line == expected_line, "subscription captures the caller line");
}

void test_return_address() {
    const ReturnAddressProbe probe = capture_return_address();
#if defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__)
    CHECK(probe.actual != nullptr, "supported compiler returns a diagnostic return address");
    CHECK(probe.actual == probe.expected,
          "diagnostic return address preserves its caller's identity");
#else
    CHECK(probe.actual == nullptr, "unsupported compiler reports no diagnostic return address");
#endif
}

}  // namespace

int main() {
    format_probe("%s", "compiler boundary");
    test_callsite_capture();
    test_return_address();
    return check_summary();
}
