#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

// Deliberately fixed, independent of the engine RNG and standard distributions.
namespace matter::fuzz {
struct Random {
    uint32_t state;
    uint32_t next() {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return state;
    }
};
struct Input {
    const uint8_t *data;
    size_t size, offset = 0;
    uint8_t byte() { return offset < size ? data[offset++] : 0; }
    uint64_t word() {
        uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) value = (value << 8) | byte();
        return value;
    }
    std::string text(size_t limit) {
        const auto count = size - offset < limit ? size - offset : limit;
        // An empty libFuzzer input may have a null pointer.
        return count ? std::string(reinterpret_cast<const char *>(data + offset), count) : std::string{};
    }
};
inline void check(bool pass, const char *message) {
    if (!pass) { std::fprintf(stderr, "FUZZ PROPERTY FAILED: %s\n", message); std::abort(); }
}
inline std::string mutate(std::string text, Input &input) {
    const auto edits = 1 + input.byte() % 8;
    for (unsigned i = 0; i < edits; ++i) {
        const auto position = input.word() % (text.size() + 1);
        const auto operation = input.byte() % 3;
        if (operation == 0) text.insert(position, 1, char(input.byte()));
        else if (position < text.size()) {
            if (operation == 1) text.erase(position, 1);
            else text[position] = char(input.byte());
        }
    }
    return text;
}
// Same corpus in the quick gate and the opt-in driver. Print seed/iteration
// BEFORE the call so a sanitizer abort gives a direct reproduction command.
template<class Fn> void seeded(uint32_t seed, size_t iterations, Fn run) {
    std::fprintf(stderr, "seed=%u edge=empty\n", seed);
    run(nullptr, 0);
    Random random{seed ? seed : 1};
    for (size_t i = 0; i < iterations; ++i) {
        uint8_t bytes[256];
        for (auto &byte : bytes) byte = uint8_t(random.next());
        // Guarantee every operation/control mode is represented by the gate.
        bytes[0] = uint8_t(i);
        std::fprintf(stderr, "seed=%u iteration=%zu\n", seed, i);
        run(bytes, sizeof(bytes));
    }
}
} // namespace matter::fuzz
