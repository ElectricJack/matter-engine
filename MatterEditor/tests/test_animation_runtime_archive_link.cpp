#include "dsl_bindings.h"
#include "dsl_state.h"
#include "quickjs.h"

#include <string>

// This intentionally crosses the same static-archive boundary as the Linux
// Viewer.  install_bindings() retains dsl_bindings.o, and end_clip() retains
// dsl_animation.o.  The link line below supplies Ozz runtime archives only;
// a runtime archive must therefore not retain any offline Ozz dependency.
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
    state.begin_clip("probe", 1.0f, 30.0f, false, false);
    state.end_clip();

    const bool rejected_for_bake_host = state.has_error() &&
        state.error().find("animation bake host") != std::string::npos;
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return rejected_for_bake_host ? 0 : 1;
}
