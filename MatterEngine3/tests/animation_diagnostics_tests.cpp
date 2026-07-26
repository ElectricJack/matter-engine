// Phase D1 — the bake must report EVERY animation diagnostic, not just the first.
//
// BakeResult historically carried a single BakeError, and every animation failure
// path collapsed its Diagnostics list with `diagnostics.items.front()`. An author
// whose rig had four problems saw one of them, fixed it, rebaked, and saw the next.
// These tests pin the full list surviving the bake boundary with its code, message,
// and source span intact, in the stable order the validator sorted it into.
#include "check.h"
#include "script_host.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

script_host::BakeResult bake(const std::string& body, script_host::ScriptHost& host) {
    return host.bake_source("class DiagPart extends Part { build(p) {\n" + body + "\n} }", "{}", {});
}

// A rig whose motion graph declares two targets that BOTH write the tip joint.
// v1 rejects overlapping writable chains, and the validator reports one
// diagnostic per offending declaration rather than stopping at the first.
const char* kOverlappingTargets =
    "this.beginRig('r'); this.root('root'); this.bone('mid',[1,0,0]); this.bone('tip',[1,0,0]); this.endRig();"
    "this.beginClip('idle',{duration:1,sampleRate:1}); this.key('root',0,{}); this.endClip();"
    "this.beginMotion();"
    "this.target('a',{start:'root',end:'tip',driver:'external',pole:[0,0,1]});"
    "this.target('b',{start:'root',end:'tip',driver:'external',pole:[0,0,1]});"
    "this.clipNode('idleNode','idle'); this.output('out','idleNode'); this.endMotion();";

void test_bake_reports_every_animation_diagnostic() {
    script_host::ScriptHost host;
    const auto result = bake(kOverlappingTargets, host);
    std::printf("  bake error: [%s] %s @ %s\n", result.error.code.c_str(),
                result.error.message.c_str(), result.error.source_location.c_str());
    CHECK(!result.error.ok, "an invalid animation build still fails closed");

    // The single-message BakeError stays exactly as it was: existing callers and
    // their expectations are unchanged by this addition.
    CHECK(!result.error.message.empty(), "the legacy single-message error is still populated");

    CHECK(!result.animation_diagnostics.empty(),
          "the bake reports its animation diagnostics rather than discarding them");
    if (result.animation_diagnostics.empty()) return;

    for (const auto& diagnostic : result.animation_diagnostics) {
        CHECK(!diagnostic.code.empty(), "every reported diagnostic carries its code");
        CHECK(!diagnostic.message.empty(), "every reported diagnostic carries its message");
    }

    // The first reported diagnostic must be the one the legacy error collapsed to,
    // so a caller reading either surface agrees about what failed first.
    CHECK(result.animation_diagnostics.front().message == result.error.message,
          "the legacy error message is the first reported diagnostic");

    std::printf("  reported %zu animation diagnostic(s):\n", result.animation_diagnostics.size());
    for (const auto& diagnostic : result.animation_diagnostics)
        std::printf("    [%s] %s (%s:%u:%u)\n", diagnostic.code.c_str(), diagnostic.message.c_str(),
                    diagnostic.module.c_str(), diagnostic.line, diagnostic.column);
}

void test_diagnostic_order_is_stable_across_bakes() {
    script_host::ScriptHost first_host, second_host;
    const auto first = bake(kOverlappingTargets, first_host);
    const auto second = bake(kOverlappingTargets, second_host);
    CHECK(first.animation_diagnostics.size() == second.animation_diagnostics.size(),
          "the same source reports the same diagnostic count");
    bool identical = first.animation_diagnostics.size() == second.animation_diagnostics.size();
    for (size_t i = 0; identical && i < first.animation_diagnostics.size(); ++i) {
        const auto& a = first.animation_diagnostics[i];
        const auto& b = second.animation_diagnostics[i];
        if (a.code != b.code || a.message != b.message || a.module != b.module ||
            a.line != b.line || a.column != b.column || a.object != b.object)
            identical = false;
    }
    CHECK(identical, "diagnostic order and content are stable across bakes");
}

void test_successful_bake_reports_no_diagnostics() {
    script_host::ScriptHost host;
    const auto result = bake(
        "this.beginRig('r'); this.root('root'); this.bone('mid',[1,0,0]); this.bone('tip',[1,0,0]); this.endRig();"
        "this.beginClip('idle',{duration:1,sampleRate:1}); this.key('root',0,{}); this.endClip();"
        "this.beginMotion(); this.clipNode('idleNode','idle'); this.output('out','idleNode'); this.endMotion();",
        host);
    if (!result.error.ok)
        std::printf("  unexpected failure: %s (%s)\n", result.error.message.c_str(), result.error.code.c_str());
    CHECK(result.error.ok, "the valid control rig bakes");
    CHECK(result.animation_diagnostics.empty(),
          "a successful bake reports no animation diagnostics");
}

} // namespace

int main() {
    test_bake_reports_every_animation_diagnostic();
    test_diagnostic_order_is_stable_across_bakes();
    test_successful_bake_reports_no_diagnostics();
    return check_summary();
}
