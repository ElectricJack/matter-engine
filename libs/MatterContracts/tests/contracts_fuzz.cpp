#include "fuzz_properties.h"
#include <charconv>

// Byte inputs exercise strict parsing/decoding and a valid generated document
// with structural mutations. The latter keeps deep validator paths reachable
// even when libFuzzer starts without a corpus. Crashes print area and seed.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 8192) return 0;
  uint64_t seed = UINT64_C(0x6d6174746572);
  for (size_t i = 0; i < size; ++i) seed = (seed ^ data[i]) * UINT64_C(0x100000001b3);
  properties::current_area = "bytes";
  properties::current_seed = seed;
  const std::string_view text(size ? reinterpret_cast<const char *>(data) : "", size);
  properties::json_bytes(text);
  const auto options = properties::bounded_options();
  properties::decode_bytes(text, properties::recipe(seed), options);
  properties::decode_bytes(text, properties::asset(seed), options);
  if ((seed >> 1) & 1) properties::recipe_case(seed);
  else properties::asset_case(seed);
  return 0;
}

#ifndef MATTER_CONTRACTS_LIBFUZZER
int main(int argc, char **argv) {
  uint64_t iterations = 10000, seed = UINT64_C(0x66757a7a);
  properties::require(argc <= 3, "usage: matter_contracts_fuzz [iterations] [decimal seed]");
  auto parse = [](const char *s, uint64_t &out) {
    const auto result = std::from_chars(s, s + std::strlen(s), out);
    properties::require(result.ec == std::errc{} && *result.ptr == '\0', "expected unsigned decimal argument");
  };
  if (argc >= 2) parse(argv[1], iterations);
  if (argc >= 3) parse(argv[2], seed);
  matter::contracts::Rng rng(seed);
  for (uint64_t i = 0; i < iterations; ++i) {
    std::string bytes;
    // Mix valid trees, document-shaped JSON, and arbitrary/truncated bytes.
    if (i % 4 == 0) bytes = properties::canonical(properties::generated_value(rng));
    else if (i % 4 == 1) bytes = properties::wire(properties::recipe(rng.next_u64()));
    else if (i % 4 == 2) bytes = properties::wire(properties::asset(rng.next_u64()), properties::bounded_options());
    else {
      bytes.resize(rng.next_u64() % 512);
      for (auto &c : bytes) c = static_cast<char>(rng.next_u64() & 255);
    }
    if (i % 8 == 1 && !bytes.empty()) bytes.resize(rng.next_u64() % bytes.size());
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
  }
  std::cout << "PASS standalone iterations=" << iterations << " seed=" << seed << '\n';
}
#endif
