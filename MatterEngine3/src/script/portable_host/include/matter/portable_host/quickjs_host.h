#pragma once
#include <matter/contracts/hosts.h>

namespace matter::portable_host {
struct QuickJsOptions {
  contracts::ValidationOptions validation;
  uint32_t max_modules = 1024;
  uint64_t max_source_bytes = 8ull * 1024 * 1024;
  uint32_t stack_bytes = 512 * 1024;
};

// Each call owns a fresh runtime. Inputs/callbacks are borrowed synchronously;
// the returned immutable recipe survives the runtime and the input snapshot.
// The default ES module export is (context) => RecipeDocument (or a Promise).
// context contains source, parameters, uint64 BigInt seed, services, limits,
// supportedCapabilities. Import the embedded SDK as 'matter:authoring'.
// Supplied modules are identified by canonical semantic IDs; relative imports
// resolve against those IDs and must be listed in SourceModule.dependencies.
// No filesystem, network, bake, cache, world or graphics services are installed.
std::unique_ptr<services::AuthoringHost> make_quickjs_host(QuickJsOptions = {});

// Include this identity alongside the transitive snapshot in authoring cache
// keys. It covers the embedded SDK, shared contract helpers and host JS bridge.
contracts::ContentDigest quickjs_sdk_content();
} // namespace matter::portable_host
