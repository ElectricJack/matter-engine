#include <matter/portable_host/quickjs_host.h>
#include "../../../../../tests/fuzz/support.h"

namespace {
namespace d = matter::contracts;
namespace s = matter::services;
namespace h = matter::portable_host;
namespace f = matter::fuzz;
using E = d::ErrorCode;
const std::string good = R"JS(import {RecipeBuilder} from 'matter:authoring';
export default ctx => {const b = new RecipeBuilder(ctx);
b.output(b.box('box', [1,2,3])); return b.build();};)JS";
s::SourceSnapshot snapshot(std::string source) {
    auto digest = d::sha256(source);
    s::SourceModule module{{{"property/main.mjs"}, "fixture://property.mjs", "seeded", digest, {}}, std::move(source), {}};
    return {{"property/main.mjs"}, std::move(digest), {std::move(module)}, {}};
}
void inspect(const s::RecipeResult &result, const s::JobEnvelope &job) {
    f::check(result.envelope() == job, "host preserves complete job envelope");
    if (result.ok()) {
        f::check(bool(result.value()), "successful authoring owns recipe");
        std::string wire;
        f::check(d::encode(*result.value(), wire).ok(), "published recipe independently encodes");
        d::RecipeDocument copy;
        f::check(d::decode_recipe(wire, copy).ok(), "published recipe independently decodes");
    } else {
        f::check(!result.value(), "host failure publishes no partial recipe");
        bool error = false;
        for (const auto &diagnostic : result.diagnostics()) if (diagnostic.severity == d::Severity::error) error = true;
        f::check(error, "host failure has error diagnostics");
    }
}
void failed(const s::RecipeResult &result, E expected) {
    f::check(!result.ok(), "controlled failure rejected");
    for (const auto &diagnostic : result.diagnostics()) if (diagnostic.code == expected) return;
    for (const auto &diagnostic : result.diagnostics())
        std::fprintf(stderr, "%s: %s\n", d::code_name(diagnostic.code), diagnostic.message.c_str());
    f::check(false, "expected controlled failure code");
}
void succeeded(const s::RecipeResult &result, const char *message) {
    if (!result.ok()) for (const auto &diagnostic : result.diagnostics())
        std::fprintf(stderr, "%s: %s\n", d::code_name(diagnostic.code), diagnostic.message.c_str());
    f::check(result.ok(), message);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    f::Input input{data, size};
    const auto mode = input.byte() % 10;
    const auto supplied_epoch = input.word();
    const auto epoch = supplied_epoch ? supplied_epoch : 1; // successful jobs require a nonzero epoch
    const s::JobEnvelope job{"host.property", "revision.seeded", epoch};
    h::QuickJsOptions options;
    options.max_modules = 8;
    auto host = h::make_quickjs_host(options);
    s::ResourceBudget budget;
    // Bound arbitrary programs by work, heap and wall time. The quick suite
    // uses mostly structured sources; longer mutation stays opt-in.
    budget.working_bytes = 8 << 20;
    budget.execution_microseconds = 25000;
    budget.operations = 4000;
    auto source = snapshot(good);
    s::JobControl control;
    s::CancellationSource cancellation;
    E expected = E::invalid_input;
    bool expect_failure = false;
    switch (mode) {
    case 0: source = snapshot(input.text(2048)); break;
    case 1: source = snapshot(f::mutate(good, input)); break;
    case 2: {
        const char *values[] = {"undefined", "NaN", "Infinity", "{get a(){throw Error('getter')}}",
            "(()=>{const a={};a.self=a;return a})()", "[,,,]", "{text:'\\ud800'}", "()=>1"};
        source = snapshot("export default () => (" + std::string(values[input.byte() % 8]) + ");");
        break;
    }
    case 3: {
        const auto dimension = std::to_string(1 + input.byte());
        auto generated = good;
        generated.replace(generated.find("[1,2,3]"), 7, "[" + dimension + ",2,3]");
        source = snapshot(std::move(generated));
        // Use the contract's normal deadline for known-valid SDK programs.
        // Scheduling/coverage overhead must not masquerade as a data failure.
        budget.execution_microseconds = s::ResourceBudget{}.execution_microseconds;
        s::AuthoringRequest request; request.seed = epoch;
        const auto first = host->author(job, source, request, budget, {});
        const auto second = host->author(job, source, request, budget, {});
        succeeded(first, "generated host program succeeds"); succeeded(second, "repeated host program succeeds");
        f::check(first.value()->seed == epoch && second.value()->seed == epoch, "host preserves full uint64 seed");
        inspect(first, job); inspect(second, job);
        std::string left, right;
        f::check(d::encode(*first.value(), left).ok() && d::encode(*second.value(), right).ok() && left == right,
                 "fresh runtime deterministic output");
        host.reset(); source.modules.clear();
        f::check(first.value()->nodes.size() == 1, "recipe survives input and host destruction");
        return 0;
    }
    case 4:
        source.modules[0].dependencies = {{"property/missing.mjs"}};
        expected = E::missing_dependency; expect_failure = true; break;
    case 5:
        source.modules[0].dependencies = {{"property/main.mjs"}};
        expected = E::cycle; expect_failure = true; break;
    case 6:
        // Coverage instrumentation makes SDK setup slower; cancellation is the
        // oracle here, so do not let the arbitrary-program deadline win first.
        budget.execution_microseconds = s::ResourceBudget{}.execution_microseconds;
        if (input.byte() % 2) cancellation.cancel();
        control = {cancellation.token(), [&](const s::Progress &) { cancellation.cancel(); }};
        expected = E::cancelled; expect_failure = true; break;
    case 7:
        budget.working_bytes = 1024 + input.byte();
        expected = E::resource_limit; expect_failure = true; break;
    case 8: {
        const char *loops[] = {"for(;;){}", "export default () => {for(;;){}}",
            "export default () => new Promise(()=>{function spin(){Promise.resolve().then(spin)}spin()});"};
        source = snapshot(loops[input.byte() % 3]);
        expected = E::resource_limit; expect_failure = true; break;
    }
    case 9:
        source.modules[0].source_utf8 += char(input.byte()); // digest deliberately stays unchanged
        expected = E::invalid_input; expect_failure = true; break;
    }
    const auto result = host->author(job, source, {}, budget, control);
    inspect(result, job);
    if (expect_failure) failed(result, expected);
    // Prove an aborted/failed runtime cannot poison later jobs or the SDK.
    budget = {};
    const auto recovery = host->author(job, snapshot(good), {}, budget, {});
    succeeded(recovery, "host recovers after malformed/cancelled/resource-limited input"); inspect(recovery, job);
    return 0;
}
#ifndef MATTER_FUZZ_DRIVER
int main() {
    matter::fuzz::seeded(0x514a5348, 30, LLVMFuzzerTestOneInput);
    std::puts("ALL PASS: seeded portable-host properties");
}
#endif
