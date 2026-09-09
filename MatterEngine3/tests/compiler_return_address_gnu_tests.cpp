#include "matter/compiler.h"

#if !defined(__GNUC__) && !defined(__clang__)
#error This fixture requires a GNU-family compiler
#endif

struct ReturnAddressProbe {
    void* expected;
    void* actual;
};

__attribute__((noinline)) ReturnAddressProbe capture_return_address() {
    void* expected = __builtin_extract_return_addr(__builtin_return_address(0));
    return ReturnAddressProbe{expected, matter::diagnostics::return_address()};
}

int main() {
    const ReturnAddressProbe probe = capture_return_address();
    return probe.actual != nullptr && probe.actual == probe.expected ? 0 : 1;
}
