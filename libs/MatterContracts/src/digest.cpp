#include "internal.h"
#include <algorithm>
#include <cstring>

namespace matter::contracts {
namespace {
constexpr uint32_t constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
uint32_t rotate(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
void block(const uint8_t *b, std::array<uint32_t, 8> &h) {
  uint32_t w[64]{};
  for (size_t i = 0; i < 16; ++i)
    w[i] = (uint32_t(b[4 * i]) << 24) | (uint32_t(b[4 * i + 1]) << 16) |
           (uint32_t(b[4 * i + 2]) << 8) | b[4 * i + 3];
  for (size_t i = 16; i < 64; ++i) {
    auto a = w[i - 15], c = w[i - 2];
    w[i] = w[i - 16] + (rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3)) + w[i - 7] +
           (rotate(c, 17) ^ rotate(c, 19) ^ (c >> 10));
  }
  auto a = h[0], b0 = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6],
       h0 = h[7];
  for (size_t i = 0; i < 64; ++i) {
    auto t1 = h0 + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) +
              ((e & f) ^ ((~e) & g)) + constants[i] + w[i];
    auto t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) +
              ((a & b0) ^ (a & c) ^ (b0 & c));
    h0 = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b0;
    b0 = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b0;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += h0;
}
} // namespace
ContentDigest sha256(std::string_view bytes) {
  std::array<uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t i = 0;
  while (bytes.size() - i >= 64) {
    block(reinterpret_cast<const uint8_t *>(bytes.data() + i), h);
    i += 64;
  }
  std::array<uint8_t, 128> tail{};
  const size_t remaining = bytes.size() - i;
  if (remaining)
    std::memcpy(tail.data(), bytes.data() + i, remaining);
  tail[remaining] = 0x80;
  const size_t length = remaining < 56 ? 64 : 128;
  const uint64_t bits = static_cast<uint64_t>(bytes.size()) * 8;
  for (size_t j = 0; j < 8; ++j)
    tail[length - 1 - j] = static_cast<uint8_t>(bits >> (j * 8));
  block(tail.data(), h);
  if (length == 128)
    block(tail.data() + 64, h);
  std::string hex;
  hex.reserve(64);
  for (auto n : h)
    for (int shift = 28; shift >= 0; shift -= 4)
      hex.push_back("0123456789abcdef"[(n >> shift) & 15]);
  return {hex};
}
template <typename Document>
ValidationResult digest_document(const Document &doc, ContentDigest &out,
                                 const ValidationOptions &options) {
  auto result = validate(doc, options);
  if (!result.ok())
    return result;
  try {
    auto value = to_value(doc);
    auto &object = std::get<Value::Object>(value.data);
    object.erase("metadata");
    // Source mapping is diagnostic metadata; its edits cannot change geometry
    // identity.
    object.erase("source_map");
    auto &extensions = std::get<Value::Array>(object.at("extensions").data);
    extensions.erase(
        std::remove_if(
            extensions.begin(), extensions.end(),
            [](const Value &e) {
              return !std::get<bool>(
                  std::get<Value::Object>(e.data).at("required").data);
            }),
        extensions.end());
    std::string bytes;
    result = canonical_json(value, bytes, options.limits);
    if (result.ok())
      out = sha256(std::string(canonical_profile) + "\n" + bytes);
    return result;
  } catch (const std::bad_alloc &) {
    return detail::failure(ErrorCode::resource_limit,
                           "digest allocation failed");
  } catch (const std::exception &e) {
    return detail::failure(ErrorCode::invalid_input, e.what());
  }
}
ValidationResult content_digest(const RecipeDocument &doc, ContentDigest &out,
                                const ValidationOptions &options) {
  return digest_document(doc, out, options);
}
ValidationResult content_digest(const AssetDocument &doc, ContentDigest &out,
                                const ValidationOptions &options) {
  return digest_document(doc, out, options);
}
} // namespace matter::contracts
