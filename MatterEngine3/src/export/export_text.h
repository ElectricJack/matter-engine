#pragma once

// MatterEngine3/src/export/export_text.h
//
// The one number-to-text conversion every export writer uses.
//
// WHY NOT snprintf. The exporter's output must be BYTE-IDENTICAL for identical
// input, across compilers and across whatever locale the process happens to be
// in — that is what makes an export diffable and content-addressable. printf's
// "%f" writes the locale's decimal separator (a comma in a de_DE process), and
// its rounding of exact ties is not fixed by the C standard. This does the
// conversion in integer arithmetic from a double, rounds halves away from zero,
// and emits '.' unconditionally.
//
// Header-only and dependency-free on purpose: obj_writer, the manifest emitter
// and the tests all need it, and none of them should have to link anything for
// it.

#include <cmath>
#include <cstdint>
#include <string>

namespace matter_export {

// Fixed-point decimal with trailing zeros trimmed. `decimals` is clamped to
// [0, 9]. Non-finite input becomes "0" (an OBJ with "nan" in it is not a file
// anyone can load, and a silent 0 is at least parseable — callers that care
// validate their data before writing it). Negative zero normalises to "0", so
// two runs that differ only in a sign bit still compare equal.
inline std::string format_fixed(double value, int decimals) {
    if (decimals < 0) decimals = 0;
    if (decimals > 9) decimals = 9;
    if (!std::isfinite(value)) return "0";

    static const int64_t kPow10[10] = {1,
                                       10,
                                       100,
                                       1000,
                                       10000,
                                       100000,
                                       1000000,
                                       10000000,
                                       100000000,
                                       1000000000};
    const int64_t scale = kPow10[decimals];
    const double scaled = value * static_cast<double>(scale);
    // Anything past 2^62 / scale is far outside the metre-scale numbers this
    // exporter deals in; clamping keeps the int64 conversion defined.
    const double limit = 4.0e18;
    const double clamped = (scaled > limit) ? limit : ((scaled < -limit) ? -limit : scaled);
    int64_t units = static_cast<int64_t>(std::round(clamped));
    if (units == 0) return "0";

    const bool negative = units < 0;
    uint64_t magnitude = negative ? static_cast<uint64_t>(-(units + 1)) + 1u
                                  : static_cast<uint64_t>(units);
    const uint64_t whole = magnitude / static_cast<uint64_t>(scale);
    uint64_t fraction = magnitude % static_cast<uint64_t>(scale);

    std::string out;
    if (negative) out.push_back('-');
    out += std::to_string(whole);
    if (decimals > 0 && fraction != 0u) {
        std::string digits = std::to_string(fraction);
        // fraction < scale == 10^decimals, so this never underflows; the guard
        // is here so a future change to `scale` fails loudly rather than
        // allocating SIZE_MAX zeros.
        if (digits.size() < static_cast<size_t>(decimals))
            digits.insert(digits.begin(), static_cast<size_t>(decimals) - digits.size(), '0');
        while (!digits.empty() && digits.back() == '0') digits.pop_back();
        if (!digits.empty()) {
            out.push_back('.');
            out += digits;
        }
    }
    return out;
}

// The precisions the exporter writes at. Positions are metres, so 1e-6 is a
// micrometre; normals and UVs are unit-range, so 1e-6 is well inside 8-bit and
// 16-bit downstream quantisation either way.
inline constexpr int kPositionDecimals = 6;
inline constexpr int kNormalDecimals = 6;
inline constexpr int kUvDecimals = 6;
inline constexpr int kScalarDecimals = 6;

inline std::string format_hex64(uint64_t value) {
    static const char* kDigits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kDigits[value & 0xFu];
        value >>= 4;
    }
    return out;
}

} // namespace matter_export
