#include "dsl_bindings.h"
#include "dsl_state.h"
#include "quickjs.h"

#include <cstdio>
#include <string>

// Crosses the same static-archive boundary the editor does: install_bindings()
// retains dsl_bindings.o and end_clip() retains dsl_animation.o, which in turn
// pulls Ozz's offline builders out of the archive.
//
// This probe used to assert the OPPOSITE -- that end_clip() failed closed with
// "animation bake host" under MATTER_RUNTIME_ANIMATION_ONLY. That boundary was
// removed: a game ships the whole engine and editor, so the process that
// authors content is the process that runs it, and refusing to compile clips
// only made animated worlds unbakeable in the one binary anyone runs.
//
// What matters now is the opposite guarantee -- that a real clip actually
// COMPILES across the archive boundary, producing serialized Ozz bytes. A link
// error, a missing offline archive, or a silent fail-closed path all show up
// here rather than as an unbakeable world three layers up.
int main() {
    dsl::DslState state;
    JSRuntime* runtime = JS_NewRuntime();
    if (!runtime) return 1;
    JSContext* context = JS_NewContext(runtime);
    if (!context) {
        JS_FreeRuntime(runtime);
        return 1;
    }
    JS_SetContextOpaque(context, &state);
    dsl::install_bindings(context);

    const matter::AnimationTransform identity{{}, {0, 0, 0, 1}, {1, 1, 1}};
    state.begin_rig("probe");
    state.rig_root("root", identity);
    state.end_rig();
    // A clip with real keys, so end_clip() runs the offline skeleton and clip
    // builders rather than bailing out on an empty track list.
    state.begin_clip("probe", 1.0f, 30.0f, false, false);
    state.clip_key("root", 0.0f, identity);
    state.clip_key("root", 1.0f, identity);
    state.end_clip();

    int failures = 0;
    if (state.has_error()) {
        std::printf("FAIL: clip compilation errored: %s\n", state.error().c_str());
        ++failures;
    }
    const matter::animation::AnimationBuild* authored = state.authored_animation();
    if (!authored) {
        std::printf("FAIL: no authored animation build was retained\n");
        ++failures;
    } else {
        if (authored->ozz_skeleton_blob.empty()) {
            std::printf("FAIL: skeleton was not serialized across the archive boundary\n");
            ++failures;
        }
        if (authored->clips.empty() || authored->clips.front().ozz_blob.empty()) {
            std::printf("FAIL: clip was not serialized across the archive boundary\n");
            ++failures;
        }
    }
    if (failures == 0)
        std::printf("PASS: the archive compiles a clip to serialized Ozz bytes\n");

    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return failures == 0 ? 0 : 1;
}
