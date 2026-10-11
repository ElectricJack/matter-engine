#include <matter/contracts/contracts.h>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char **argv) {
  namespace mc = matter::contracts;
  mc::ValidationOptions options;
  for (int i = 1; i < argc; ++i) options.supported_capabilities.push_back({argv[i], 1});
  const std::string input{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
  mc::RecipeDocument document;
  auto result = mc::decode_recipe(input, document, options);
  std::string canonical;
  if (result.ok()) result = mc::encode(document, canonical, options);
  if (!result.ok()) {
    for (const auto &d : result.diagnostics)
      std::cerr << mc::code_name(d.code) << ": " << d.node_id << ": " << d.message << '\n';
    return 1;
  }
  std::cout << canonical << '\n';
  return 0;
}
