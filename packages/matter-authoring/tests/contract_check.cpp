#include <matter/contracts/contracts.h>
#include <iostream>
#include <iterator>
#include <string>
#include "sdk_sources.h"

int main(int argc, char **argv) {
  namespace mc = matter::contracts;
  if (argc == 2 && std::string(argv[1]) == "--sdk-sources") {
    mc::Value::Object modules;
    for (const auto &module : sdk_modules)
      modules.emplace(module.name, std::string(reinterpret_cast<const char *>(module.source), module.size));
    std::string output;
    if (!mc::canonical_json(mc::Value(std::move(modules)), output).ok()) return 1;
    std::cout << output;
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "--batch") {
    const std::string input{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
    mc::Value decoded;
    if (!mc::parse_json(input, decoded).ok()) return 1;
    mc::Value::Array results;
    for (const auto &entry : std::get<mc::Value::Array>(decoded.data)) {
      const auto &object = std::get<mc::Value::Object>(entry.data);
      mc::ValidationOptions options;
      if (std::get<bool>(object.at("legacy").data))
        options.supported_capabilities.push_back({"matter.legacy.voxel-meshing", 1});
      std::string wire, canonical;
      mc::RecipeDocument document;
      if (!mc::canonical_json(object.at("document"), wire).ok() ||
          !mc::decode_recipe(wire, document, options).ok() ||
          !mc::encode(document, canonical, options).ok()) return 1;
      results.emplace_back(std::move(canonical));
    }
    std::string output;
    if (!mc::canonical_json(mc::Value(std::move(results)), output).ok()) return 1;
    std::cout << output;
    return 0;
  }
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
