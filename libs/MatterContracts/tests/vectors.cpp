#include "fixtures.h"
#include <cstring>
#include <iostream>
#include <limits>
namespace mc = matter::contracts;
std::string hex(uint64_t n) {
  std::string text(16, '0');
  for (int i = 15; i >= 0; --i) {
    text[static_cast<size_t>(i)] = "0123456789abcdef"[n & 15];
    n >>= 4;
  }
  return text;
}
int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--examples") {
    std::string wire;
    auto r = mc::encode(fixtures::recipe(), wire);
    if (!r.ok())
      return 1;
    std::cout << wire << '\n';
    r = mc::encode(fixtures::asset(), wire);
    if (!r.ok())
      return 1;
    std::cout << wire << '\n';
    return 0;
  }
  mc::Value::Array random;
  for (uint64_t seed :
       {UINT64_C(0), UINT64_C(1), UINT64_C(0x0123456789abcdef), UINT64_MAX}) {
    mc::Rng rng(seed);
    mc::Value::Array words, units;
    for (int i = 0; i < 8; ++i)
      words.emplace_back(hex(rng.next_u64()));
    mc::Rng floats(seed);
    for (int i = 0; i < 8; ++i)
      units.emplace_back(floats.next_unit());
    random.emplace_back(mc::Value::Object{
        {"seed", hex(seed)}, {"words", words}, {"units", units}});
  }
  mc::Value::Array cases;
  std::vector<double> numbers{0,
                              -0.0,
                              0.1,
                              -0.1,
                              1.25,
                              1e-7,
                              1e21,
                              std::numeric_limits<double>::min(),
                              std::numeric_limits<double>::denorm_min(),
                              std::numeric_limits<double>::max(),
                              9007199254740992.0};
  // Bit patterns, including subnormals and exponent transitions, avoid testing
  // only easy literals; JS reconstructs each from the published hex bits.
  uint64_t state = UINT64_C(0x7236741829349821);
  for (int i = 0; i < 256; ++i) {
    state = state * UINT64_C(6364136223846793005) + 1;
    uint64_t bits = state;
    if (((bits >> 52) & 2047) == 2047)
      bits ^= UINT64_C(0x0010000000000000);
    double n = 0;
    std::memcpy(&n, &bits, 8);
    numbers.push_back(n);
  }
  for (double n : numbers) {
    uint64_t bits = 0;
    std::memcpy(&bits, &n, 8);
    std::string canonical;
    auto r = mc::canonical_json(mc::Value(n), canonical);
    if (!r.ok())
      return 2;
    mc::Value parsed;
    if (!mc::parse_json(canonical, parsed).ok() ||
        std::get<double>(parsed.data) != n)
      return 6;
    cases.emplace_back(
        mc::Value::Object{{"bits", hex(bits)},
                          {"canonical", canonical},
                          {"sha256", mc::sha256(canonical).hex}});
  }
  const auto recipe = fixtures::recipe();
  const auto asset = fixtures::asset();
  mc::ContentDigest recipe_digest, asset_digest;
  if (!mc::content_digest(recipe, recipe_digest).ok() ||
      !mc::content_digest(asset, asset_digest).ok())
    return 3;
  std::string recipe_wire, asset_wire;
  if (!mc::encode(recipe, recipe_wire).ok() ||
      !mc::encode(asset, asset_wire).ok())
    return 4;
  std::string optional_wire;
  if (!mc::encode(fixtures::optional_asset(), optional_wire,
                  fixtures::optional_options())
           .ok())
    return 7;
  mc::Value object = mc::Value::Object{{"rng", random},
                                       {"numbers", cases},
                                       {"recipe_wire", recipe_wire},
                                       {"recipe_digest", recipe_digest.hex},
                                       {"asset_wire", asset_wire},
                                       {"asset_digest", asset_digest.hex},
                                       {"optional_wire", optional_wire}};
  std::string output;
  if (!mc::canonical_json(object, output).ok())
    return 5;
  std::cout << output << '\n';
}
