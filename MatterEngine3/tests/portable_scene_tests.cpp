// PortableScene (component_adapters/graphics) against a live VulkanDevice
// (clear-crest-48.9.8). Products come from the REAL authoring route, and every
// assertion reads what PortableScene itself captured with VulkanRaster
// (MATTER_PORTABLE_SCENE_CAPTURE): the draws, their pixels and the resident
// assets. Covers:
//   - shared instances: two placements of one cold-baked part draw from one
//     uploaded product;
//   - dynamic material: a part whose material id is past the frozen builtins
//     is refused once and never drawn (no invented white);
//   - late product, unchanged instances: a part baked by ANOTHER process (the
//     warm resolve cache an editor restart sees) is admitted before its
//     product exists, then restored here; the next update with
//     instances_changed=false draws it;
//   - removal: dropping one shared placement keeps the product, dropping the
//     last retires it.
// Needs a Vulkan device (lavapipe is enough) and a display: the vendored GLFW
// has no null platform. Runs validated when the Khronos layer is installed.
#include "matter/windows_compat.h"
#define GLFW_INCLUDE_NONE
#include "component_adapters/graphics/portable_scene.h"
#include "component_adapters/authoring/engine/legacy_part_route.h"
#include "material_registry.h"
#include "matter/log.h"
#include "matter/vulkan_device.h"
#include "part_graph.h"
#include "render/vk_scene_renderer.h"
#include "test_sandbox.h"
#include <GLFW/glfw3.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "check.h"

namespace {
namespace aa = matter::authoring_adapter;

constexpr const char* kMarker = "  static authoring = \"matter.authoring.portable.v1\";\n";

std::string part(const char* name, const std::string& fill) {
    return std::string("class ") + name + " extends Part {\n" + kMarker + "  build(p) {\n    this.fill(" + fill +
           ");\n    this.box([0, 0, 0], [1, 1, 1]);\n  }\n}\n";
}

struct MapResolver : part_graph::ModuleResolver {
    std::map<std::string, std::string> sources;
    bool load_source(const std::string& module, std::string& out) override {
        const auto it = sources.find(module);
        if (it == sources.end()) return false;
        out = it->second;
        return true;
    }
    bool get_requires(const std::string&, const part_graph::Params&,
                      std::vector<part_graph::ChildRequest>&) override {
        return true;
    }
};

part_graph::InstallResult install(const std::string& root, MapResolver& modules,
                                  std::vector<part_graph::ChildRequest> requests) {
    script_host::ScriptHost host;
    part_graph::HostBaker baker(host, root);
    baker.set_portable_route(aa::make_legacy_part_route());
    part_graph::PartGraph graph(modules, baker);
    return graph.install(requests);
}

// --cold <parts root>: bakes Late in this (soon gone) process and prints its
// hash, leaving the parent a warm resolve cache with nothing retained.
int cold_child(const std::string& root) {
    MapResolver modules;
    modules.sources["Late"] = part("Late", "MAT.plaster");
    const auto result = install(root, modules, {{"Late", {}}});
    if (!result.ok || result.root_hashes.size() != 1 || !aa::retained_asset(result.root_hashes[0])) return 1;
    std::printf("%llu\n", (unsigned long long)result.root_hashes[0]);
    return 0;
}

std::atomic<unsigned> validation_errors{0};
bool validated = true;
std::mutex g_log_mutex;
std::string g_graphics_log;
void sink(matter::log::Level, const char* tag, const char* message, void*) {
    if (std::strstr(message, "Vulkan validation ERROR")) ++validation_errors;
    if (!tag || std::string(tag) != "graphics") return;
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_graphics_log += message;
    g_graphics_log += "\n";
}

size_t count(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}

std::string hex(uint64_t hash) {
    char out[17];
    std::snprintf(out, sizeof out, "%016llx", (unsigned long long)hash);
    return out;
}

// What one capture recorded.
struct Capture {
    bool loaded = false;
    std::string refused, assets;
    std::map<std::string, size_t> pixels; // Draw owner ("%016llx/") -> covered pixels.
    size_t draws = 0;
};

Capture read_capture(const std::string& prefix) {
    Capture c;
    std::ifstream f(prefix + ".json", std::ios::binary);
    if (!f) return c;
    std::ostringstream ss; ss << f.rdbuf();
    const std::string json = ss.str();
    const auto section = [&](const char* key, const char* next) {
        const auto a = json.find(key), b = json.find(next);
        return a == std::string::npos || b == std::string::npos ? std::string() : json.substr(a, b - a);
    };
    c.refused = section("\"refused\"", "\"refused_placements\"");
    c.assets = section("\"assets\"", "\"draws\"");
    const std::string draws = section("\"draws\"", "\"observations\"");
    static const std::regex draw("\"instance\": \"([0-9a-f]{16}/)[^\"]*\"[^}]*\"pixels\": ([0-9]+)");
    for (auto it = std::sregex_iterator(draws.begin(), draws.end(), draw); it != std::sregex_iterator(); ++it) {
        c.pixels[(*it)[1]] += std::stoul((*it)[2]);
        ++c.draws;
    }
    c.loaded = true;
    return c;
}

viewer::VkSceneInstance at(uint64_t hash, uint64_t id, float x, float y) {
    viewer::VkSceneInstance inst;
    inst.part_hash = hash;
    inst.instance_id = id;
    float* m = inst.object_to_world.m;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[3] = x; m[7] = y; m[11] = -3.0f;
    return inst;
}

struct Window {
    GLFWwindow* handle = nullptr;
    Window() {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        handle = glfwCreateWindow(64, 64, "Portable scene", nullptr, nullptr);
    }
    ~Window() { if (handle) glfwDestroyWindow(handle); }
};

// Updates until PortableScene captures a settled frame (a fresh <prefix>.json);
// only the first update carries `changed`.
Capture settle(matter::graphics_adapter::PortableScene& scene, const std::string& prefix,
               const std::vector<viewer::VkSceneInstance>& instances, bool changed,
               const matter::graphics_adapter::PortableView& view, const char* step) {
    std::filesystem::remove(prefix + ".json");
    for (int frame = 0; frame < 2000; ++frame) {
        std::string err;
        if (!scene.update(instances, changed && frame == 0, view, err)) {
            std::printf("FAIL: %s: update: %s\n", step, err.c_str());
            ++g_failures;
            return {};
        }
        if (std::filesystem::exists(prefix + ".json")) return read_capture(prefix);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("FAIL: %s: no settled capture\n", step);
    ++g_failures;
    return {};
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--cold") return cold_child(argv[2]);
#ifdef MATTER_VK_TEST_LAYER_PATH
    SetDllDirectoryA(MATTER_VK_TEST_LAYER_PATH);
    SetEnvironmentVariableA("VK_LAYER_PATH", MATTER_VK_TEST_LAYER_PATH);
#endif
    std::error_code cwd_error;
    const std::filesystem::path cwd = std::filesystem::current_path(cwd_error);
    if (cwd_error) return 2;
    if (glfwInit() != GLFW_TRUE) {
        const char* reason = nullptr;
        glfwGetError(&reason);
        std::printf("FAIL: GLFW initializes: %s\n", reason ? reason : "unknown");
        return 1;
    }
    matter::log::add_sink(sink);

    // Products. Shared and Dynamic are cold-baked here; Late by a child
    // process, so this process holds only its warm resolve cache.
    MapResolver modules;
    const int dynamic_id = MaterialRegistryStaticCount() + 1;
    modules.sources["Shared"] = part("Shared", "MAT.stone");
    modules.sources["Dynamic"] = part("Dynamic", std::to_string(dynamic_id));
    const std::string root = make_sandbox("sandbox/portable_scene");
    const auto local = install(root, modules, {{"Shared", {}}, {"Shared", {}}, {"Dynamic", {}}});
    CHECK(local.ok && local.root_hashes.size() == 3 && local.root_hashes[0] == local.root_hashes[1],
          "cold: two Shared requests resolve to one hash");
    const uint64_t shared = local.root_hashes.size() == 3 ? local.root_hashes[0] : 0;
    const uint64_t dynamic = local.root_hashes.size() == 3 ? local.root_hashes[2] : 0;
    CHECK(aa::retained_asset(shared) && aa::retained_asset(dynamic), "cold: Shared and Dynamic products retained");

    const std::string late_root = make_sandbox("sandbox/portable_scene_late");
    uint64_t late = 0;
    {
        const std::string command = std::string(argv[0]) + " --cold " + late_root;
        FILE* child = popen(command.c_str(), "r");
        unsigned long long value = 0;
        if (child && std::fscanf(child, "%llu", &value) == 1) late = value;
        const int status = child ? pclose(child) : -1;
        CHECK(status == 0 && late, "fresh process: the child cold-bakes Late");
    }
    CHECK(!aa::retained_asset(late) && aa::retained_refusal(late).empty(),
          "warm cache: Late is neither retained nor refused in this process yet");
    std::printf("hashes: shared %s dynamic %s late %s\n", hex(shared).c_str(), hex(dynamic).c_str(),
                hex(late).c_str());

    {
        Window window;
        std::string error;
        auto vk = window.handle ? matter::VulkanDevice::create(window.handle, true, error) : nullptr;
        // Validation is wanted, not required: a host without the Khronos
        // layer runs unvalidated and says so, so the zero count below is
        // never mistaken for a validated one.
        if (!vk && window.handle && error.find("VK_LAYER_KHRONOS_validation") != std::string::npos) {
            std::printf("validation layer unavailable: running without validation\n");
            validated = false;
            error.clear();
            vk = matter::VulkanDevice::create(window.handle, false, error);
        }
        CHECK(bool(vk), error.c_str());
        if (!vk) { glfwTerminate(); return 1; }

        const std::string prefix = root + "/capture";
#ifdef _WIN32
        _putenv_s("MATTER_PORTABLE_SCENE_CAPTURE", prefix.c_str());
        _putenv_s("MATTER_PORTABLE_SCENE_CAPTURE_FRAMES", "1");
#else
        setenv("MATTER_PORTABLE_SCENE_CAPTURE", prefix.c_str(), 1);
        setenv("MATTER_PORTABLE_SCENE_CAPTURE_FRAMES", "1", 1);
#endif
        matter::graphics_adapter::PortableScene scene(*vk);
        // The authoring e2e camera: orthographic x [-2,4], y [-1,6], reversed
        // depth from z=2 to z=-8, identity view.
        matter::graphics_adapter::PortableView view;
        for (float* m : {view.world_to_view.m, view.view_to_clip.m}) m[0] = m[5] = m[10] = m[15] = 1.0f;
        float* p = view.view_to_clip.m;
        p[0] = 1.0f / 3; p[3] = -1.0f / 3; p[5] = 2.0f / 7; p[7] = -5.0f / 7; p[10] = 0.1f; p[11] = 0.8f;
        view.sun_direction = {0.0f, -1.0f, 0.0f};
        view.sun_color = {1.0f, 1.0f, 1.0f};
        view.width = view.height = 128;

        const std::string a = hex(1) + "/", b = hex(2) + "/", d = hex(3) + "/", l = hex(4) + "/";
        std::vector<viewer::VkSceneInstance> instances{
            at(shared, 1, -0.9f, 0.0f), at(shared, 2, 1.5f, 0.0f), at(dynamic, 3, 3.0f, 3.5f), at(late, 4, 0.3f, 3.5f)};

        Capture c = settle(scene, prefix, instances, true, view, "admit");
        std::printf("admit: %zu draws, A %zu px, B %zu px\n", c.draws, c.pixels[a], c.pixels[b]);
        CHECK(c.loaded && c.draws == 2 && c.pixels[a] > 0 && c.pixels[b] > 0,
              "shared instances: both placements draw");
        CHECK(count(c.assets, "\"part_hash\"") == 1 && count(c.assets, hex(shared)) == 1,
              "shared instances: one uploaded product");
        CHECK(count(c.refused, hex(dynamic)) == 1 && !c.pixels.count(d),
              "dynamic material: refused and never drawn");
        CHECK(!c.pixels.count(l) && count(c.refused, hex(late)) == 0,
              "late product: absent, not refused, before it exists");

        // Fresh-process warm restore of Late's pinned source, then an update
        // that does NOT report an instance change.
        const uint64_t generation = aa::retained_generation();
        {
            script_host::ScriptHost host;
            part_graph::HostBaker baker(host, late_root);
            baker.set_portable_route(aa::make_legacy_part_route());
            part_graph::BakeInputs inputs;
            inputs.module = "Late";
            inputs.source = part("Late", "MAT.plaster");
            std::string err;
            CHECK(part_graph::reauthor_portable(baker, {{late, inputs}}, err) && aa::retained_asset(late),
                  "warm restore re-authors Late's product");
            if (!err.empty()) std::printf("  restore: %s\n", err.c_str());
        }
        CHECK(aa::retained_generation() != generation, "warm restore signals a new product");
        c = settle(scene, prefix, instances, false, view, "late");
        std::printf("late: %zu draws, Late %zu px\n", c.draws, c.pixels[l]);
        CHECK(c.loaded && c.draws == 3 && c.pixels[l] > 0 && c.pixels[a] > 0 && c.pixels[b] > 0,
              "late product with unchanged instances: drawn on the next update");
        CHECK(count(c.assets, "\"part_hash\"") == 2 && count(c.assets, hex(late)) == 1,
              "late product: uploaded once beside Shared");

        instances.erase(instances.begin() + 1);
        c = settle(scene, prefix, instances, true, view, "remove one");
        std::printf("remove one: %zu draws\n", c.draws);
        CHECK(c.loaded && c.draws == 2 && c.pixels[a] > 0 && !c.pixels.count(b) && c.pixels[l] > 0,
              "removal: the dropped shared placement stops drawing");
        CHECK(count(c.assets, hex(shared)) == 1, "removal: the product stays while a placement uses it");

        instances.erase(instances.begin());
        c = settle(scene, prefix, instances, true, view, "remove last");
        std::printf("remove last: %zu draws\n", c.draws);
        CHECK(c.loaded && c.draws == 1 && c.pixels[l] > 0 && !c.pixels.count(a),
              "removal: the last shared placement stops drawing");
        CHECK(count(c.assets, hex(shared)) == 0 && count(c.assets, "\"part_hash\"") == 1,
              "removal: the unused product is retired");
        CHECK(count(c.refused, hex(dynamic)) == 1, "dynamic material: still refused");
    }
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        CHECK(count(g_graphics_log, "portable part " + hex(dynamic) + " stays legacy-only") == 1,
              "dynamic material: the refusal is logged exactly once");
    }
    std::printf("validation errors: %u%s\n", validation_errors.load(), validated ? "" : " (unvalidated)");
    CHECK(validation_errors == 0, "no Vulkan validation errors");
    matter::log::remove_sink(sink);
    glfwTerminate();
    std::filesystem::current_path(cwd, cwd_error);
    return check_summary();
}
