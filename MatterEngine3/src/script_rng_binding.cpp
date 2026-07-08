#include "script_rng_binding.h"
#include <cctype>
#include <cstdlib>

namespace script_rng {

static uint32_t rotl(uint32_t x, int k){ return (x << k) | (x >> (32 - k)); }

ScriptRng::ScriptRng(uint32_t seed) {
    uint32_t z = seed;
    for (int i = 0; i < 4; ++i) {
        z += 0x9e3779b9u;
        uint32_t w = z;
        w = (w ^ (w >> 16)) * 0x21f0aaadu;
        w = (w ^ (w >> 15)) * 0x735a2d97u;
        s[i] = w ^ (w >> 15);
    }
}

uint32_t ScriptRng::next_u32() {
    uint32_t result = rotl(s[1] * 5u, 7) * 9u;
    uint32_t t = s[1] << 9;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t;
    s[3] = rotl(s[3], 11);
    return result;
}

double ScriptRng::random() { return next_u32() / 4294967296.0; }

uint32_t seed_from_params_json(const std::string& j, const std::string& key) {
    // Minimal: find "key" : <digits>. Good enough for flat params blobs; SP-2's
    // structured route bypasses this and constructs ScriptRng(seed) directly.
    std::string needle = "\"" + key + "\"";
    size_t k = j.find(needle);
    if (k == std::string::npos) return 0u;
    size_t c = j.find(':', k + needle.size());
    if (c == std::string::npos) return 0u;
    size_t i = c + 1;
    while (i < j.size() && std::isspace((unsigned char)j[i])) ++i;
    bool neg = (i < j.size() && j[i] == '-'); if (neg) ++i;
    uint64_t v = 0; bool any = false;
    while (i < j.size() && std::isdigit((unsigned char)j[i])) { v = v * 10 + (j[i]-'0'); ++i; any = true; }
    if (!any) return 0u;
    return (uint32_t)(neg ? (uint32_t)(-(int64_t)v) : v);
}

} // namespace script_rng
