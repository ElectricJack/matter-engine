#include "support.h"
#include <cerrno>
#include <cstring>
#include <limits>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *, size_t);
namespace {
uint64_t number(const char *text) {
    char *end = nullptr;
    errno = 0;
    const auto value = std::strtoull(text, &end, 0);
    matter::fuzz::check(text[0] && text[0] != '-' && end && !*end && !errno, "invalid unsigned integer");
    return value;
}
}
int main(int argc, char **argv) {
    uint32_t seed = 0x6d617474;
    size_t iterations = 10000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            const auto value = number(argv[++i]);
            matter::fuzz::check(value <= UINT32_MAX, "seed exceeds uint32");
            seed = uint32_t(value);
        } else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            const auto value = number(argv[++i]);
            matter::fuzz::check(value <= std::numeric_limits<size_t>::max(), "iteration count exceeds size_t");
            iterations = size_t(value);
        } else {
            std::fprintf(stderr, "usage: %s [--seed N] [--iterations N]\n", argv[0]);
            return 2;
        }
    }
    matter::fuzz::seeded(seed, iterations, LLVMFuzzerTestOneInput);
    std::puts("ALL PASS: standalone fuzz properties");
}
