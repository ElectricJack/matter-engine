// Reads a recorded shot descriptor back. JSON parsing goes through QuickJS-ng,
// which the editor already links for the script host — writing a second parser
// for one file shape would be worse than borrowing the one that is here.
#include "shot_replay.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "matter/log.h"
#include "quickjs.h"

namespace viewer {

namespace {

// --- small typed getters over a JSValue object ------------------------------
// Each returns the fallback when the property is absent or the wrong type, so a
// descriptor written by an older build (or hand-trimmed by an agent) still
// loads with sensible defaults rather than failing outright.

bool get_number(JSContext* ctx, JSValueConst obj, const char* key,
                double& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsNumber(v)) {
        double tmp = 0;
        if (JS_ToFloat64(ctx, &tmp, v) == 0) { out = tmp; ok = true; }
    }
    JS_FreeValue(ctx, v);
    return ok;
}

bool get_bool(JSContext* ctx, JSValueConst obj, const char* key, bool& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsBool(v)) { out = JS_ToBool(ctx, v) != 0; ok = true; }
    JS_FreeValue(ctx, v);
    return ok;
}

bool get_string(JSContext* ctx, JSValueConst obj, const char* key,
                std::string& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsString(v)) {
        if (const char* s = JS_ToCString(ctx, v)) {
            out = s;
            JS_FreeCString(ctx, s);
            ok = true;
        }
    }
    JS_FreeValue(ctx, v);
    return ok;
}

// Reads a fixed-length numeric array property, e.g. "eye": [x, y, z].
bool get_floats(JSContext* ctx, JSValueConst obj, const char* key, float* out,
                int count) {
    JSValue arr = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsArray(arr)) {
        ok = true;
        for (int i = 0; i < count && ok; ++i) {
            JSValue item = JS_GetPropertyUint32(ctx, arr, static_cast<uint32_t>(i));
            double tmp = 0;
            if (JS_IsNumber(item) && JS_ToFloat64(ctx, &tmp, item) == 0)
                out[i] = static_cast<float>(tmp);
            else
                ok = false;
            JS_FreeValue(ctx, item);
        }
    }
    JS_FreeValue(ctx, arr);
    return ok;
}

bool get_rect(JSContext* ctx, JSValueConst obj, const char* key,
              ShotRect& out) {
    JSValue r = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsObject(r)) {
        double x = 0, y = 0, w = 0, h = 0;
        if (get_number(ctx, r, "x", x) && get_number(ctx, r, "y", y) &&
            get_number(ctx, r, "w", w) && get_number(ctx, r, "h", h)) {
            out = ShotRect{static_cast<int32_t>(x), static_cast<int32_t>(y),
                           static_cast<int32_t>(w), static_cast<int32_t>(h)};
            ok = true;
        }
    }
    JS_FreeValue(ctx, r);
    return ok;
}

std::string read_file(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { ok = false; return {}; }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    ok = true;
    return buffer.str();
}

} // namespace

ShotReplay load_shot_replay(const std::string& path, int shot_index) {
    ShotReplay replay;
    bool read_ok = false;
    const std::string text = read_file(path, read_ok);
    if (!read_ok) {
        replay.error = "could not read " + path;
        return replay;
    }

    JSRuntime* runtime = JS_NewRuntime();
    if (!runtime) { replay.error = "QuickJS runtime allocation failed"; return replay; }
    JSContext* ctx = JS_NewContext(runtime);
    if (!ctx) {
        JS_FreeRuntime(runtime);
        replay.error = "QuickJS context allocation failed";
        return replay;
    }
    // No JS_AddIntrinsicJSON here: JS_NewContext already installs it, and
    // adding it a second time on a full context is not idempotent. Only the
    // JS_NewContextRaw path in script_host.cpp has to opt in explicitly.

    JSValue root = JS_ParseJSON(ctx, text.c_str(), text.size(), path.c_str());
    if (JS_IsException(root)) {
        JS_FreeValue(ctx, root);
        JS_FreeContext(ctx);
        JS_FreeRuntime(runtime);
        replay.error = path + ": not valid JSON";
        return replay;
    }

    JSValue shots = JS_GetPropertyStr(ctx, root, "shots");
    if (!JS_IsArray(shots)) {
        JS_FreeValue(ctx, shots);
        JS_FreeValue(ctx, root);
        JS_FreeContext(ctx);
        JS_FreeRuntime(runtime);
        replay.error = path + ": no \"shots\" array";
        return replay;
    }
    uint32_t count = 0;
    {
        JSValue length = JS_GetPropertyStr(ctx, shots, "length");
        double tmp = 0;
        if (JS_ToFloat64(ctx, &tmp, length) == 0) count = static_cast<uint32_t>(tmp);
        JS_FreeValue(ctx, length);
    }
    if (shot_index < 1 || static_cast<uint32_t>(shot_index) > count) {
        JS_FreeValue(ctx, shots);
        JS_FreeValue(ctx, root);
        JS_FreeContext(ctx);
        JS_FreeRuntime(runtime);
        replay.error = path + ": shot " + std::to_string(shot_index) +
                       " out of range (" + std::to_string(count) + " recorded)";
        return replay;
    }

    JSValue shot = JS_GetPropertyUint32(ctx, shots,
                                        static_cast<uint32_t>(shot_index - 1));
    double n = 0;
    get_string(ctx, shot, "world", replay.world);
    get_rect(ctx, shot, "rect", replay.rect);
    get_rect(ctx, shot, "viewport", replay.viewport);
    replay.has_viewport_uv =
        get_floats(ctx, shot, "viewport_uv", replay.viewport_uv, 4);
    std::string layout_file;
    if (get_string(ctx, shot, "layout", layout_file) && !layout_file.empty()) {
        // Relative to the descriptor, so a report directory can be moved or
        // copied around as a unit.
        const std::filesystem::path sidecar =
            std::filesystem::path(path).parent_path() / layout_file;
        bool layout_ok = false;
        const std::string text = read_file(sidecar.string(), layout_ok);
        if (layout_ok)
            replay.layout_ini = text;
        else
            MATTER_LOGW("replay", "layout sidecar %s is missing\n",
                        sidecar.string().c_str());
    }
    {
        float fb[2] = {0, 0};
        if (get_floats(ctx, shot, "framebuffer", fb, 2)) {
            replay.frame_width = static_cast<uint32_t>(fb[0]);
            replay.frame_height = static_cast<uint32_t>(fb[1]);
        }
    }
    {
        JSValue camera = JS_GetPropertyStr(ctx, shot, "camera");
        if (JS_IsObject(camera)) {
            get_floats(ctx, camera, "eye", replay.eye, 3);
            get_floats(ctx, camera, "target", replay.target, 3);
            get_floats(ctx, camera, "up", replay.up, 3);
            if (get_number(ctx, camera, "fov_radians", n))
                replay.fov_radians = static_cast<float>(n);
            if (get_number(ctx, camera, "near", n))
                replay.near_plane = static_cast<float>(n);
            if (get_number(ctx, camera, "far", n))
                replay.far_plane = static_cast<float>(n);
        }
        JS_FreeValue(ctx, camera);
    }
    {
        JSValue sim = JS_GetPropertyStr(ctx, shot, "sim");
        if (JS_IsObject(sim)) {
            get_string(ctx, sim, "mode", replay.sim_mode);
            if (get_number(ctx, sim, "time_scale", n))
                replay.time_scale = static_cast<float>(n);
        }
        JS_FreeValue(ctx, sim);
    }
    {
        JSValue render = JS_GetPropertyStr(ctx, shot, "render");
        if (JS_IsObject(render)) {
            get_string(ctx, render, "dlss", replay.dlss_mode);
            if (get_number(ctx, render, "pixel_budget", n))
                replay.pixel_budget = static_cast<float>(n);
            // "resolver" is READ AND DISCARDED for old descriptors: every
            // shot on disk records one, and there is only one resolver now,
            // so the recorded choice can neither be honoured nor is it needed.
            // Parsing past it silently is what keeps those shots replayable.
            if (get_number(ctx, render, "debug_view", n))
                replay.debug_view_mode = static_cast<int>(n);
            get_bool(ctx, render, "ui_visible", replay.ui_visible);
        }
        JS_FreeValue(ctx, render);
    }

    JS_FreeValue(ctx, shot);
    JS_FreeValue(ctx, shots);
    JS_FreeValue(ctx, root);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);

    if (replay.world.empty()) {
        replay.error = path + ": shot " + std::to_string(shot_index) +
                       " has no world; cannot replay";
        return replay;
    }
    replay.valid = true;
    return replay;
}

ShotReplay load_replay_from_env() {
    ShotReplay replay;
    const char* path = std::getenv("MATTER_REPLAY");
    if (!path || !path[0]) return replay;  // not a replay run; error stays empty
    int index = 1;
    if (const char* which = std::getenv("MATTER_REPLAY_SHOT")) {
        const int parsed = std::atoi(which);
        if (parsed > 0) index = parsed;
    }
    return load_shot_replay(path, index);
}

} // namespace viewer
