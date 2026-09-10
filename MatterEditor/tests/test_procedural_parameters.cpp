#include "../src/procedural_parameters.h"

#include <cstdio>
#include <cstdlib>

namespace {
using matter::jsondoc::Value;
namespace procedural = viewer::procedural;

#define CHECK(c, m) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); std::exit(1); } } while (false)

Value parse(const char* text) {
    Value value;
    CHECK(matter::jsondoc::parse_json(text, value), "fixture JSON parses");
    return value;
}

procedural::Root root() {
    procedural::Root value;
    value.object.kind = viewer::agent::ObjectIdentity::Kind::BakedRoot;
    value.object.id = 42;
    value.module = "Terrain";
    value.source_path = "objects/Terrain.js";
    value.params_json = "{\"enabled\":true,\"height\":12,\"worldSeed\":7}";
    return value;
}

void test_schema_is_typed_and_honest_about_ranges() {
    const Value result = procedural::describe_json(root());
    const Value* available = result.find("available");
    const Value* fields = result.find("parameters");
    CHECK(available && available->b, "valid root parameters are discoverable");
    CHECK(fields && fields->arr.size() == 3, "every declared key is returned");
    const Value* range = fields->arr[1].find("range");
    CHECK(range && range->find("available") && !range->find("available")->b,
          "unknown range metadata is explicit rather than invented");
}

void test_invalid_plan_does_not_mutate_the_output() {
    procedural::Plan plan;
    plan.changed.push_back("sentinel");
    std::string error;
    CHECK(!procedural::make_plan(root(), parse("{\"height\":13,\"missing\":1}"),
                                 plan, error), "unknown field is rejected");
    CHECK(plan.changed.size() == 1 && plan.changed[0] == "sentinel",
          "a rejected multi-field update leaves the previous plan untouched");
    CHECK(!procedural::make_plan(root(), parse("{\"enabled\":2}"), plan, error),
          "type mismatch is rejected");
    CHECK(!procedural::make_plan(root(), parse("{\"height\":1e999}"), plan, error),
          "non-finite numeric input is rejected");
}

void test_plan_is_atomic_and_preserves_seed_as_integer() {
    procedural::Plan plan;
    std::string error;
    CHECK(procedural::make_plan(root(), parse("{\"height\":13,\"worldSeed\":99}"),
                                plan, error), "valid multi-field update plans");
    CHECK(plan.changed.size() == 2, "all changed fields are reported together");
    const Value* seed = plan.after.find("worldSeed");
    CHECK(seed && seed->kind == Value::Kind::Number && seed->num == 99,
          "deterministic seed is preserved in the planned canonical object");
    const Value receipt = procedural::plan_json(plan, true);
    CHECK(receipt.find("dry_run") && receipt.find("dry_run")->b,
          "dry-run receipt is explicit");
    CHECK(receipt.find("persistence") &&
          receipt.find("persistence")->str == "session_only_root_override",
          "the source-preserving persistence mode is reported");
}
}  // namespace

int main() {
    test_schema_is_typed_and_honest_about_ranges();
    test_invalid_plan_does_not_mutate_the_output();
    test_plan_is_atomic_and_preserves_seed_as_integer();
    std::puts("procedural parameter tests: PASS");
    return 0;
}
