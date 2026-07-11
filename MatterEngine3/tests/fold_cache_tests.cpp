// fold_cache_tests.cpp — ScriptHost fold cache hit/miss counting and clear.
// Tests that fold_sources is cached by (source, shared-lib root) key and
// that resolve_hash sees stable results across cache hits.
#include "check.h"
#include "../src/script_host.h"
#include <string>

static const char* kTinyPart = R"JS(
import { rng } from 'shared-lib/rng';
class Tiny extends Part {
  static params = { seed: 1 };
  build(p) { const r = rng(p.seed); this.pushMatrix(); this.popMatrix(); }
}
)JS";

int main() {
    script_host::ScriptHost host;
    host.set_shared_lib_root("../shared-lib");

    CHECK(host.fold_cache_misses() == 0 && host.fold_cache_hits() == 0,
          "counters start at zero");

    uint64_t h1 = host.resolve_hash(kTinyPart, "{}");
    CHECK(host.fold_cache_misses() == 1, "first fold is a miss");

    uint64_t h2 = host.resolve_hash(kTinyPart, "{}");
    CHECK(h1 == h2, "hash stable across cached fold");
    CHECK(host.fold_cache_hits() >= 1, "second fold hits the cache");
    CHECK(host.fold_cache_misses() == 1, "no second miss for same source");

    std::string other = std::string(kTinyPart) + "\n// different\n";
    host.resolve_hash(other, "{}");
    CHECK(host.fold_cache_misses() == 2, "different source is a new miss");

    host.clear_fold_cache();
    host.resolve_hash(kTinyPart, "{}");
    CHECK(host.fold_cache_misses() == 3, "clear_fold_cache invalidates");

    return check_summary();
}
