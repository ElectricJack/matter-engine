#include "fuzz_properties.h"
#include <charconv>

namespace p = properties;
namespace mc = matter::contracts;
void json_tests() {
  p::current_area = "json";
  p::require(p::canonical(mc::Value::Object{{"\xc3\xa9", 3}, {"z", 2}, {"a", 1}})
                 == "{\"a\":1,\"z\":2,\"\xc3\xa9\":3}", "canonical keys follow UTF-8 byte order");
  p::require(p::canonical(mc::Value("\n")) == "\"\\u000a\"", "canonical control escapes use lowercase hex");
  mc::Rng rng(UINT64_C(0xc0dec0de));
  for (unsigned i = 0; i < 96; ++i) {
    p::current_seed = i;
    const auto v = p::generated_value(rng);
    p::json_bytes(p::canonical(v));
    // Unbiased finite binary64 bit patterns cover extreme exponents and
    // subnormals, independently of the small document-generation numbers.
    auto bits = rng.next_u64();
    double number = 0;
    std::memcpy(&number, &bits, sizeof(number));
    if (std::isfinite(number)) {
      const auto text = p::canonical(mc::Value(number));
      mc::Value decoded;
      p::require(mc::parse_json(text, decoded).ok(), "exact binary64 parses");
      p::require(std::get<double>(decoded.data) == number, "binary64 value survives exactly");
      p::require(p::canonical(decoded) == text, "binary64 canonical identity");
    }
    std::string arbitrary(rng.next_u64() % 128, '\0');
    for (auto &c : arbitrary) c = static_cast<char>(rng.next_u64() & 255);
    p::json_bytes(arbitrary);
  }
  for (double n : {0.0, -0.0, std::numeric_limits<double>::denorm_min(),
                   -std::numeric_limits<double>::denorm_min(),
                   std::numeric_limits<double>::min(), std::numeric_limits<double>::max()})
    p::json_bytes(p::canonical(mc::Value(n)));
  for (const char *text : {"", "[", "{\"a\":0,\"a\":1}", "01", "1e9999",
                           "\"\\ud800\"", "\"\\udfff\"", "\"\\ud800\\u0000\"", "[1,]"}) {
    mc::Value sentinel = "unchanged";
    p::require(!mc::parse_json(text, sentinel).ok(), "malformed corpus rejected");
    p::require(std::get<std::string>(sentinel.data) == "unchanged", "malformed corpus parse is atomic");
  }
  for (const std::string &s : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
                              std::string("\xf4\x90\x80\x80"), std::string("\x80")}) {
    std::string out = "unchanged";
    p::require(!mc::canonical_json(mc::Value(s), out).ok() && out == "unchanged",
               "invalid UTF-8 writer rejection is atomic");
    mc::Value sentinel = "unchanged";
    p::require(!mc::parse_json("\"" + s + "\"", sentinel).ok()
                   && std::get<std::string>(sentinel.data) == "unchanged",
               "invalid UTF-8 parser rejection is atomic");
  }
  // Test inclusive byte/value/depth limits and one-below failures.
  const mc::Value tree = mc::Value::Array{mc::Value(mc::Value::Array{1, "\n"})};
  const auto bytes = p::canonical(tree);
  mc::ValidationLimits limits;
  limits.max_depth = 2; limits.max_value_nodes = 4;
  limits.max_document_bytes = bytes.size(); limits.max_string_bytes = 1;
  mc::Value out;
  std::string text;
  p::require(mc::canonical_json(tree, text, limits).ok(), "inclusive writer budgets pass");
  p::require(mc::parse_json(bytes, out, limits).ok(), "inclusive parser budgets pass");
  for (unsigned which = 0; which < 4; ++which) {
    p::current_seed = which;
    auto small = limits;
    if (which == 0) --small.max_depth;
    if (which == 1) --small.max_value_nodes;
    if (which == 2) --small.max_document_bytes;
    if (which == 3) --small.max_string_bytes;
    text = "unchanged"; out = "unchanged";
    p::require(p::has_code(mc::canonical_json(tree, text, small), mc::ErrorCode::resource_limit)
                   && text == "unchanged", "writer budget fails atomically");
    p::require(p::has_code(mc::parse_json(bytes, out, small), mc::ErrorCode::resource_limit)
                   && std::get<std::string>(out.data) == "unchanged", "parser budget fails atomically");
  }
}
int main(int argc, char **argv) {
  p::require(argc == 2 || argc == 3, "usage: matter_contracts_properties json|recipe|asset [decimal seed]");
  const std::string area = argv[1];
  if (area == "json") json_tests();
  else if (area == "recipe" || area == "asset") {
    if (argc == 3) {
      uint64_t seed = 0;
      const auto result = std::from_chars(argv[2], argv[2] + std::strlen(argv[2]), seed);
      p::require(result.ec == std::errc{} && *result.ptr == '\0', "expected unsigned decimal seed");
      if (area == "recipe") p::recipe_case(seed);
      else p::asset_case(seed);
      return 0;
    }
    for (uint64_t i = 0; i < 8; ++i) {
      const auto seed = UINT64_C(0x6d61747465720000) + i;
      if (area == "recipe") p::recipe_case(seed);
      else p::asset_case(seed);
    }
  } else p::require(false, "unknown corpus");
  std::cout << "PASS deterministic " << area << " properties\n";
}
