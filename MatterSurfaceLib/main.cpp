extern "C" {
    #include "raylib.h"
    #include "rlgl.h"
}

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

extern "C" {
    GLFWwindow* glfwGetCurrentContext();
    void glFinish(void);
}

#include "include/blas_manager.hpp"
#include "include/tlas_manager.hpp"
#include "include/bvh_visualizer.hpp"
#include "include/bvh_analyzer.h"
#include "include/cluster.h"
#include "include/cell.h"
#include "include/profiler.hpp"
#include "include/material_registry.h"
#include "include/particle_culling.h"

// ---------------------------------------------------------------------------
// In-app GL program-binary cache.
//
// Under WSLg, OpenGL is emulated by Mesa's d3d12 driver, so the 1200+ line
// raytrace uber-shader is translated GLSL->NIR->DXIL and recompiled by the
// NVIDIA D3D12 backend on every launch -- tens of seconds of CPU work. The
// linked program binary (ARB_get_program_binary, core since GL 4.1) lets us
// store the driver's compiled result on disk and restore it on the next launch
// with glProgramBinary, skipping the translate+compile entirely.
//
// The cache key includes the shader source text and the GL_VERSION/GL_RENDERER
// strings, so a driver upgrade or shader edit produces a new key. As a final
// guard, a restored binary is only accepted if it reports GL_LINK_STATUS ==
// GL_TRUE; otherwise we silently fall back to compiling from source.
// ---------------------------------------------------------------------------
#ifndef GL_PROGRAM_BINARY_LENGTH
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#endif
#ifndef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_TRUE
#define GL_TRUE 1
#endif
#define MSL_GL_VERTEX_SHADER   0x8B31
#define MSL_GL_FRAGMENT_SHADER 0x8B30

namespace {

typedef void   (*MSL_glGetProgramiv)(unsigned int, unsigned int, int*);
typedef void   (*MSL_glGetProgramBinary)(unsigned int, int, int*, unsigned int*, void*);
typedef void   (*MSL_glProgramBinary)(unsigned int, unsigned int, const void*, int);
typedef unsigned int (*MSL_glCreateProgram)(void);
typedef void   (*MSL_glDeleteProgram)(unsigned int);
typedef void   (*MSL_glAttachShader)(unsigned int, unsigned int);
typedef void   (*MSL_glDetachShader)(unsigned int, unsigned int);
typedef void   (*MSL_glDeleteShader)(unsigned int);
typedef void   (*MSL_glBindAttribLocation)(unsigned int, unsigned int, const char*);
typedef void   (*MSL_glProgramParameteri)(unsigned int, unsigned int, int);
typedef void   (*MSL_glLinkProgram)(unsigned int);

struct GLBinFns {
    MSL_glGetProgramiv     getProgramiv     = nullptr;
    MSL_glGetProgramBinary getProgramBinary = nullptr;
    MSL_glProgramBinary    programBinary    = nullptr;
    MSL_glCreateProgram    createProgram    = nullptr;
    MSL_glDeleteProgram    deleteProgram    = nullptr;
    MSL_glAttachShader     attachShader     = nullptr;
    MSL_glDetachShader     detachShader     = nullptr;
    MSL_glDeleteShader     deleteShader     = nullptr;
    MSL_glBindAttribLocation bindAttribLocation = nullptr;
    MSL_glProgramParameteri  programParameteri  = nullptr;
    MSL_glLinkProgram      linkProgram      = nullptr;
    bool ok() const {
        return getProgramiv && getProgramBinary && programBinary && createProgram &&
               deleteProgram && attachShader && detachShader && deleteShader &&
               bindAttribLocation && programParameteri && linkProgram;
    }
};

GLBinFns load_gl_bin_fns() {
    GLBinFns f;
    f.getProgramiv       = (MSL_glGetProgramiv)      glfwGetProcAddress("glGetProgramiv");
    f.getProgramBinary   = (MSL_glGetProgramBinary)  glfwGetProcAddress("glGetProgramBinary");
    f.programBinary      = (MSL_glProgramBinary)     glfwGetProcAddress("glProgramBinary");
    f.createProgram      = (MSL_glCreateProgram)     glfwGetProcAddress("glCreateProgram");
    f.deleteProgram      = (MSL_glDeleteProgram)     glfwGetProcAddress("glDeleteProgram");
    f.attachShader       = (MSL_glAttachShader)      glfwGetProcAddress("glAttachShader");
    f.detachShader       = (MSL_glDetachShader)      glfwGetProcAddress("glDetachShader");
    f.deleteShader       = (MSL_glDeleteShader)      glfwGetProcAddress("glDeleteShader");
    f.bindAttribLocation = (MSL_glBindAttribLocation)glfwGetProcAddress("glBindAttribLocation");
    f.programParameteri  = (MSL_glProgramParameteri) glfwGetProcAddress("glProgramParameteri");
    f.linkProgram        = (MSL_glLinkProgram)       glfwGetProcAddress("glLinkProgram");
    return f;
}

// raylib's GL 3.3 default vertex shader (mirrors rlgl.h defaultVShaderCode), so
// a program we link ourselves matches the batch's vertex attribute layout.
const char* kDefaultVShader =
    "#version 330                       \n"
    "in vec3 vertexPosition;            \n"
    "in vec2 vertexTexCoord;            \n"
    "in vec4 vertexColor;               \n"
    "out vec2 fragTexCoord;             \n"
    "out vec4 fragColor;                \n"
    "uniform mat4 mvp;                  \n"
    "void main()                        \n"
    "{                                  \n"
    "    fragTexCoord = vertexTexCoord; \n"
    "    fragColor = vertexColor;       \n"
    "    gl_Position = mvp*vec4(vertexPosition, 1.0); \n"
    "}                                  \n";

uint64_t fnv1a64(const char* s) {
    uint64_t h = 1469598103934665603ULL;
    for (; s && *s; ++s) { h ^= (unsigned char)(*s); h *= 1099511628211ULL; }
    return h;
}

// Populate the default attribute/uniform locations exactly as raylib's
// LoadShaderFromMemory does, so a binary-restored program draws the fullscreen
// quad (vertex pos/texcoord/color + mvp + colDiffuse + texture0) correctly.
void fill_default_shader_locs(Shader* sh) {
    sh->locs = (int*)RL_CALLOC(RL_MAX_SHADER_LOCATIONS, sizeof(int));
    for (int i = 0; i < RL_MAX_SHADER_LOCATIONS; i++) sh->locs[i] = -1;
    sh->locs[SHADER_LOC_VERTEX_POSITION]   = rlGetLocationAttrib(sh->id, "vertexPosition");
    sh->locs[SHADER_LOC_VERTEX_TEXCOORD01] = rlGetLocationAttrib(sh->id, "vertexTexCoord");
    sh->locs[SHADER_LOC_VERTEX_TEXCOORD02] = rlGetLocationAttrib(sh->id, "vertexTexCoord2");
    sh->locs[SHADER_LOC_VERTEX_NORMAL]     = rlGetLocationAttrib(sh->id, "vertexNormal");
    sh->locs[SHADER_LOC_VERTEX_TANGENT]    = rlGetLocationAttrib(sh->id, "vertexTangent");
    sh->locs[SHADER_LOC_VERTEX_COLOR]      = rlGetLocationAttrib(sh->id, "vertexColor");
    sh->locs[SHADER_LOC_MATRIX_MVP]        = rlGetLocationUniform(sh->id, "mvp");
    sh->locs[SHADER_LOC_MATRIX_VIEW]       = rlGetLocationUniform(sh->id, "matView");
    sh->locs[SHADER_LOC_MATRIX_PROJECTION] = rlGetLocationUniform(sh->id, "matProjection");
    sh->locs[SHADER_LOC_MATRIX_MODEL]      = rlGetLocationUniform(sh->id, "matModel");
    sh->locs[SHADER_LOC_MATRIX_NORMAL]     = rlGetLocationUniform(sh->id, "matNormal");
    sh->locs[SHADER_LOC_COLOR_DIFFUSE]     = rlGetLocationUniform(sh->id, "colDiffuse");
    sh->locs[SHADER_LOC_MAP_DIFFUSE]       = rlGetLocationUniform(sh->id, "texture0");
    sh->locs[SHADER_LOC_MAP_SPECULAR]      = rlGetLocationUniform(sh->id, "texture1");
    sh->locs[SHADER_LOC_MAP_NORMAL]        = rlGetLocationUniform(sh->id, "texture2");
}

struct ShaderBinHeader {
    uint32_t magic;    // 'MSLB'
    uint32_t version;  // bump to invalidate all caches on format change
    uint32_t format;   // GLenum binaryFormat from glGetProgramBinary
    uint32_t length;   // payload byte count
    uint64_t key;      // source + GL version hash (sanity check)
};
const uint32_t kShaderBinMagic   = 0x424C534Du; // "MSLB"
const uint32_t kShaderBinVersion = 1u;

} // namespace

// Load a fragment shader (raylib default vertex shader) using an on-disk
// program-binary cache. On a cache hit the linked program is restored without
// recompiling; on a miss it compiles via raylib LoadShader and saves the binary
// for next time. Always returns a usable Shader (falls back to plain LoadShader
// if the cache or GL extension is unavailable).
static Shader LoadShaderCached(const char* fsPath) {
    static GLBinFns fns = load_gl_bin_fns();

    char* src = LoadFileText(fsPath);
    if (!src || !fns.ok()) {
        if (src) UnloadFileText(src);
        return LoadShader(nullptr, fsPath);
    }

    // Key = shader source + GL identity, so driver/shader changes miss cleanly.
    const char* glVer = (const char*)glGetString(0x1F02 /*GL_VERSION*/);
    const char* glRen = (const char*)glGetString(0x1F01 /*GL_RENDERER*/);
    std::string keyStr = src;
    if (glVer) { keyStr += "\n##GL_VERSION="; keyStr += glVer; }
    if (glRen) { keyStr += "\n##GL_RENDERER="; keyStr += glRen; }
    uint64_t key = fnv1a64(keyStr.c_str());

    char cachePath[512];
    snprintf(cachePath, sizeof(cachePath), ".shader_cache/%016llx.glprog",
             (unsigned long long)key);

    // --- Try cache hit -----------------------------------------------------
    FILE* in = fopen(cachePath, "rb");
    if (in) {
        ShaderBinHeader hdr{};
        if (fread(&hdr, sizeof(hdr), 1, in) == 1 &&
            hdr.magic == kShaderBinMagic && hdr.version == kShaderBinVersion &&
            hdr.key == key && hdr.length > 0) {
            std::vector<unsigned char> bin(hdr.length);
            if (fread(bin.data(), 1, hdr.length, in) == hdr.length) {
                unsigned int prog = fns.createProgram();
                fns.programBinary(prog, hdr.format, bin.data(), (int)hdr.length);
                int linked = 0;
                fns.getProgramiv(prog, GL_LINK_STATUS, &linked);
                if (linked == GL_TRUE) {
                    fclose(in);
                    UnloadFileText(src);
                    Shader sh{};
                    sh.id = prog;
                    fill_default_shader_locs(&sh);
                    TraceLog(LOG_INFO, "SHADER: Restored program from binary cache: %s", cachePath);
                    return sh;
                }
                fns.deleteProgram(prog); // rejected (e.g. driver upgrade) -> recompile
            }
        }
        fclose(in);
    }

    // --- Cache miss: compile + link ourselves so we can request a retrievable
    // binary (raylib links without GL_PROGRAM_BINARY_RETRIEVABLE_HINT, which
    // makes glGetProgramBinary return a zero-length blob on Mesa). ------------
    unsigned int vs = rlCompileShader(kDefaultVShader, MSL_GL_VERTEX_SHADER);
    unsigned int fs = rlCompileShader(src, MSL_GL_FRAGMENT_SHADER);
    UnloadFileText(src);

    if (vs == 0 || fs == 0) {
        if (vs) fns.deleteShader(vs);
        if (fs) fns.deleteShader(fs);
        return LoadShader(nullptr, fsPath); // recover via raylib's own path
    }

    unsigned int prog = fns.createProgram();
    fns.attachShader(prog, vs);
    fns.attachShader(prog, fs);
    // Bind attribute locations to raylib's defaults so the batch VAO matches.
    fns.bindAttribLocation(prog, 0, "vertexPosition");
    fns.bindAttribLocation(prog, 1, "vertexTexCoord");
    fns.bindAttribLocation(prog, 2, "vertexNormal");
    fns.bindAttribLocation(prog, 3, "vertexColor");
    fns.bindAttribLocation(prog, 4, "vertexTangent");
    fns.bindAttribLocation(prog, 5, "vertexTexCoord2");
    fns.programParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    fns.linkProgram(prog);

    int linked = 0;
    fns.getProgramiv(prog, GL_LINK_STATUS, &linked);
    fns.detachShader(prog, vs);
    fns.detachShader(prog, fs);
    fns.deleteShader(vs);
    fns.deleteShader(fs);

    if (linked != GL_TRUE) {
        fns.deleteProgram(prog);
        return LoadShader(nullptr, fsPath); // fall back to raylib (it logs the error)
    }

    Shader sh{};
    sh.id = prog;
    fill_default_shader_locs(&sh);

    int len = 0;
    fns.getProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len > 0) {
        std::vector<unsigned char> bin(len);
        int written = 0;
        unsigned int format = 0;
        fns.getProgramBinary(prog, len, &written, &format, bin.data());
        if (written > 0) {
            if (!DirectoryExists(".shader_cache")) MakeDirectory(".shader_cache");
            FILE* out = fopen(cachePath, "wb");
            if (out) {
                ShaderBinHeader hdr{kShaderBinMagic, kShaderBinVersion, format,
                                    (uint32_t)written, key};
                fwrite(&hdr, sizeof(hdr), 1, out);
                fwrite(bin.data(), 1, written, out);
                fclose(out);
                TraceLog(LOG_INFO, "SHADER: Saved program binary to cache (%d bytes): %s",
                         written, cachePath);
            }
        }
    } else {
        TraceLog(LOG_WARNING, "SHADER: Program binary not retrievable; cache disabled this run");
    }
    return sh;
}

class MatterSurfaceLibDemo {
public:
    MatterSurfaceLibDemo(int width, int height) 
        : screen_width_(width), screen_height_(height),
          blas_manager_(std::make_unique<BLASManager>()),
          tlas_manager_(std::make_unique<TLASManager>(4096)),
          bvh_visualizer_(std::make_unique<BVHVisualizer>()),
          test_cluster_(std::make_unique<Cluster>(0, *blas_manager_, *tlas_manager_, 5.0f)) {
        
        InitWindow(screen_width_, screen_height_, "MatterSurfaceLib - Cluster and Cell System");
        SetTargetFPS(60);

        // Start in UI interaction mode (cursor enabled) for immediate ImGui access
        cursor_disabled_ = false;
        EnableCursor();
        
        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        
        // Setup Dear ImGui style
        ImGui::StyleColorsDark();
        
        // Setup Platform/Renderer backends
        GLFWwindow* window = glfwGetCurrentContext();
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330");
        
        setup_rendering();
        // setup_matter_system();
        setup_lattice_scene();
        
        // Initialize BVH analysis system
        setup_bvh_analysis();
    }
    
    ~MatterSurfaceLibDemo() {
        cleanup();
        
        // ImGui Cleanup
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        
        EnableCursor();
        CloseWindow();
    }
    
    void run() {
        // Print initial BLAS/TLAS statistics
        print_rendering_stats();

        // Non-interactive capture mode for automated visual debugging: render one
        // framed shot at a given simplification ratio to a PNG, then exit.
        if (const char* cap = getenv("MSL_CAPTURE")) {
            run_capture(cap);
            return;
        }

        // Pay the one-time raytrace shader compile now, so the first toggle into
        // raytrace mode doesn't freeze the loop. See warm_up_raytracing_shader().
        warm_up_raytracing_shader();

        while (!WindowShouldClose()) {
            PROFILE_FRAME_BEGIN();
                update();
                render();
            PROFILE_FRAME_END();
            
            // Print performance stats every 60 frames (roughly once per second at 60 FPS)
            // static int frame_counter = 0;
            // frame_counter++;
            // if (frame_counter >= 60) {
            //     printf("\n=== FRAME %d PERFORMANCE REPORT ===\n", frame_counter);
            //     PROFILE_PRINT();
            //     print_rendering_stats();
                
            //     // Reset profiler stats to show per-period performance instead of cumulative
            //     PROFILE_RESET();
            //     frame_counter = 0;
            // }
        }
    }

    // Render a single framed screenshot at a chosen simplification ratio, then return.
    // Controlled by env vars so an external harness can drive it:
    //   MSL_CAPTURE     output PNG path (presence enables this mode)
    //   MSL_RATIO       simplification ratio to apply (default 1.0)
    //   MSL_RENDER_MODE 0=raytrace 1=solid 2=wireframe 3=debug-bvh (default 1)
    //   MSL_FRAMES      frames to render before the shot (default 24)
    //   MSL_CAM         "px,py,pz,tx,ty,tz" camera override
    void run_capture(const char* out_path) {
        capture_mode_ = true;
        show_meshes_  = true;

        float ratio  = getenv("MSL_RATIO")       ? (float)atof(getenv("MSL_RATIO")) : 1.0f;
        int   mode   = getenv("MSL_RENDER_MODE")  ? atoi(getenv("MSL_RENDER_MODE"))  : 1;
        int   frames = getenv("MSL_FRAMES")       ? atoi(getenv("MSL_FRAMES"))       : 24;
        render_mode_ = mode;
        if (getenv("MSL_DEBUG_TRI")) debug_triangle_tests_ = true;

        // Default view frames the two-sphere blob (world centers ~(0,2,0) and (12,2,0), r=6).
        camera_.position = {6.0f, 16.0f, 34.0f};
        camera_.target   = {6.0f, 2.0f, 0.0f};
        camera_.up       = {0.0f, 1.0f, 0.0f};
        if (const char* cam = getenv("MSL_CAM")) {
            sscanf(cam, "%f,%f,%f,%f,%f,%f",
                   &camera_.position.x, &camera_.position.y, &camera_.position.z,
                   &camera_.target.x,   &camera_.target.y,   &camera_.target.z);
        }

        test_cluster_->set_simplification_ratio(ratio);
        // MSL_LOD lets a capture reproduce a specific LOD level (default scene
        // boots at LOD 1). Switch before the rebuild so the whole scene meshes
        // at the requested cell size.
        if (const char* lodEnv = getenv("MSL_LOD")) {
            test_cluster_->set_lod_level(atoi(lodEnv), true);
        }
        test_cluster_->force_rebuild_all_cells();

        // Reproduce the interactive "add particles" path headlessly: add N random
        // particles in MSL_ADD_BATCHES batches, rebuilding dirty cells after each
        // batch (matching the UI button), so a capture exercises incremental
        // re-meshing. Used to verify the deep-BVH / shader-stack-depth fix.
        if (const char* addp = getenv("MSL_ADD_PARTICLES")) {
            int total   = atoi(addp);
            int batches = getenv("MSL_ADD_BATCHES") ? atoi(getenv("MSL_ADD_BATCHES")) : 1;
            if (batches < 1) batches = 1;
            int per_batch = total / batches;
            float add_radius = getenv("MSL_ADD_RADIUS") ? (float)atof(getenv("MSL_ADD_RADIUS")) : 0.5f;
            SetRandomSeed(1234); // deterministic scene for pixel-comparable captures
            for (int b = 0; b < batches; ++b) {
                for (int i = 0; i < per_batch; ++i) {
                    Vector3 new_pos = {
                        GetRandomValue(-50, 50) / 10.0f,
                        GetRandomValue(-50, 50) / 10.0f,
                        GetRandomValue(-50, 50) / 10.0f};
                    uint32_t material = GetRandomValue(0, 7);
                    test_cluster_->add_particle(new_pos, add_radius, material);
                }
                test_cluster_->rebuild_dirty_cells();
            }
            printf("[capture] added %d particles in %d batches; BLAS=%d tris=%d\n",
                   per_batch * batches, batches,
                   blas_manager_->get_unique_blas_count(),
                   blas_manager_->get_total_triangle_count());
        }

        printf("[capture] ratio=%.3f mode=%d frames=%d -> %s\n", ratio, mode, frames, out_path);

        for (int i = 0; i < frames; ++i) {
            render();
        }
        TakeScreenshot(out_path);
        printf("[capture] wrote %s (%d meshes, %d tris drawn)\n",
               out_path, last_meshes_rendered_, last_triangles_rendered_);
    }

private:
    bool capture_mode_ = false;


    void setup_matter_system() {
        // Create a cluster of particles to demonstrate the system
        printf("Setting up matter system with cluster and cells...\n");
        
        // Add particles in a roughly spherical distribution - First cluster
        // for (int i = 0; i < 150; ++i) {
        //     float angle1 = (float)i * 0.05f;
        //     float angle2 = (float)i * 0.025f;
            
        //     Vector3 position = {
        //         cosf(angle1) * sinf(angle2) * 10.0f,
        //         20 + sinf(angle1) * sinf(angle2) * 10.0f,
        //         cosf(angle2) * 10.0f
        //     };
            
        //     // Cycle through first 4 materials (0-3): Red metallic, Blue diffuse, Green ground, Gold metallic
        //     uint32_t material = (i / 20) % 4;
        //     test_cluster_->add_particle(position, 1.0f, material);
        // }

        // // Add particles in a roughly spherical distribution - Second cluster
        // for (int i = 0; i < 150; ++i) {
        //     float angle1 = (float)i * 0.15f;
        //     float angle2 = (float)i * 0.035f;
            
        //     Vector3 position = {
        //         5 + cosf(angle1) * sinf(angle2) * 10.0f,
        //         -sinf(angle1) * sinf(angle2) * 10.0f,
        //         cosf(angle2) * 10.0f
        //     };
             
        //     // Cycle through materials 4-7: Glass, Emissive light, Green glass, Water
        //     uint32_t material = 4 + ((i / 20) % 4);
        //     test_cluster_->add_particle(position, 1.0f, material);
        // }


        test_cluster_->add_particle({0,0,0},  6, 0);
        test_cluster_->add_particle({12,0,0},  6, 7);
        //test_cluster_->add_particle({9,0,0},  2, 0);
        //test_cluster_->add_particle({11,0,0}, 1, 0);
        
        // Add some additional particles in a line
        // for (int i = 0; i < 10; ++i) {
        //     Vector3 position = {(float)i - 10.0f, 0.0f, 0.0f};
        //     test_cluster_->add_particle(position, 0.8f, 1);
        // }
        
        printf("Added %u particles to cluster\n", test_cluster_->get_particle_count());
        
        // Position cluster in world space
        test_cluster_->set_position({0.0f, 2.0f, 0.0f});
        
        // Force initial mesh rebuild
        test_cluster_->set_lod_level(1);
        test_cluster_->rebuild_dirty_cells();
        
        printf("Cluster has %u cells, %u dirty\n",
               test_cluster_->get_cell_count(), test_cluster_->get_dirty_cell_count());
    }

    void setup_lattice_scene() {
        printf("Setting up lattice brick scene...\n");

        // --- Tunables ---
        const int   DIM_X = 20, DIM_Y = 20, DIM_Z = 20;  // solid block of slots
        // SPACING is the lattice pitch; BASE_RADIUS is independent so spheres
        // overlap (radius > SPACING/2). Jitter and size variation break up the
        // regular packing so the surface reads as bumpy stone, not a grid.
        const float SPACING     = 0.8f;
        float BASE_RADIUS = 0.6f;             // overlap factor = 2*r/SPACING = 1.5
        float POS_JITTER  = 0.35f * SPACING;  // per-axis position jitter
        float RADIUS_VAR  = 0.5f;             // radius +/-50% (clustered)
        float CLUSTER_FREQ = 0.25f;           // low freq -> big clumps of one scale
        float VOID_AMT    = 0.28f;            // carve slots where noise < this (0=solid)
        const float VOID_FREQ = 0.12f;        // low freq -> a few big bites, not swiss cheese
        const float TINT_ALPHA  = 0.2f;
        const uint32_t MAT_OPAQUE_A = 8;  // stone_light (GROUP_STONE)
        const uint32_t MAT_OPAQUE_B = 9;  // stone_dark  (GROUP_STONE)

        // Env overrides for quick visual iteration.
        if (const char* e = getenv("MSL_BASE_RADIUS"))  { float v = (float)atof(e); if (v > 0.0f) BASE_RADIUS = v; }
        if (const char* e = getenv("MSL_JITTER"))       { float v = (float)atof(e); if (v >= 0.0f) POS_JITTER = v; }
        if (const char* e = getenv("MSL_SIZE_VAR"))     { float v = (float)atof(e); if (v >= 0.0f) RADIUS_VAR = v; }
        if (const char* e = getenv("MSL_CLUSTER_FREQ")) { float v = (float)atof(e); if (v >= 0.0f) CLUSTER_FREQ = v; }
        if (const char* e = getenv("MSL_VOID"))         { float v = (float)atof(e); if (v >= 0.0f) VOID_AMT = v; }

        // Default margin = 2 (conservatively safe). Set MSL_CULL_MARGIN to tune;
        // MSL_CULL_MARGIN=-1 bypasses culling (emit every slot) for A/B compare.
        int margin = 2;
        const char* mEnv = getenv("MSL_CULL_MARGIN");
        bool bypass = false;
        if (mEnv) { margin = atoi(mEnv); if (margin < 0) bypass = true; }

        // Skip-meshing cell size (~3 slots/cell so the meshed shell hugs the
        // surface and the interior is skippable). Sweep via MSL_CELL_SIZE.
        float cell_size = 2.4f;
        const char* csEnv = getenv("MSL_CELL_SIZE");
        if (csEnv) { float v = (float)atof(csEnv); if (v > 0.0f) cell_size = v; }
        test_cluster_->set_smallest_cell_size(cell_size);

        // Build a solid block of occupancy, centered on the origin, with a
        // checkerboard of the two opaque stones so the surface shows variation.
        GridLattice lattice(SPACING);
        Occupancy occ;
        for (int ix = 0; ix < DIM_X; ++ix)
        for (int iy = 0; iy < DIM_Y; ++iy)
        for (int iz = 0; iz < DIM_Z; ++iz) {
            // Carve organic voids: leave a slot empty where the low-frequency
            // noise field dips below VOID_AMT, so the brick has missing chunks.
            if (VOID_AMT > 0.0f &&
                lattice_vnoise(ix * VOID_FREQ, iy * VOID_FREQ, iz * VOID_FREQ) < VOID_AMT)
                continue;
            uint32_t mat = ((ix + iy + iz) & 1) ? MAT_OPAQUE_A : MAT_OPAQUE_B;
            occ.set(SlotCoord{ix, iy, iz}, SlotData{mat});
        }

        // Re-center offset: GridLattice puts slot 0 at the origin; shift by half
        // the block extent so the brick is centered. The cull must bucket slots
        // on the SAME grid the Cluster uses, so it gets this offset and the
        // cluster's cell size (LOD 0 -> smallest_cell_size).
        float halfx = (DIM_X - 1) * SPACING * 0.5f;
        float halfy = (DIM_Y - 1) * SPACING * 0.5f;
        float halfz = (DIM_Z - 1) * SPACING * 0.5f;

        CullParams p;
        p.margin = margin; p.base_radius = BASE_RADIUS;
        p.radius_variation = RADIUS_VAR;
        p.radius_cluster_freq = CLUSTER_FREQ;
        p.jitter_amount = POS_JITTER; p.tint_alpha = TINT_ALPHA; p.seed = 1337;
        p.cell_size = test_cluster_->get_smallest_cell_size();   // LOD 0 cell size
        p.cell_origin_offset = Vector3{ -halfx, -halfy, -halfz };

        CullStats stats;
        std::vector<SlotCoord> no_mesh;
        std::vector<EmittedParticle> emitted =
            bypass ? emit_all(lattice, occ, p)
                   : cull_interior(lattice, occ, p, &stats, &no_mesh);

        for (auto& ep : emitted) {
            Vector3 pos = { ep.position.x - halfx, ep.position.y - halfy, ep.position.z - halfz };
            test_cluster_->add_particle(pos, ep.radius, ep.materialId, ep.tint);
        }

        if (bypass) {
            test_cluster_->set_no_mesh_cells({});  // mesh everything
            printf("[cull] occupied=%zu emitted=%zu (margin=%d, BYPASS)\n",
                   occ.count(), emitted.size(), margin);
        } else {
            std::vector<Vector3> nm;
            nm.reserve(no_mesh.size());
            for (const SlotCoord& c : no_mesh)
                nm.push_back(Vector3{(float)c.x, (float)c.y, (float)c.z});
            test_cluster_->set_no_mesh_cells(nm);
            printf("[cull] occupied=%zu emitted=%zu cells_meshed=%zu "
                   "cells_skipped=%zu cells_core=%zu (margin=%d)\n",
                   occ.count(), emitted.size(), stats.cells_meshed,
                   stats.cells_skipped, stats.cells_core, margin);
        }

        test_cluster_->set_position({0.0f, 2.0f, 0.0f});
        test_cluster_->set_lod_level(0);
        test_cluster_->rebuild_dirty_cells();

        printf("Brick has %u cells, %u dirty\n",
               test_cluster_->get_cell_count(), test_cluster_->get_dirty_cell_count());
    }

    void setup_bvh_analysis() {
        // Register TLAS for analysis
        BVHReportManager::RegisterTLAS("Main TLAS", tlas_manager_->get_tlas());
        
        // Register all BLAS structures for analysis
        register_all_blas_for_analysis();
        
        // Initial analysis update
        BVHReportManager::UpdateAllAnalyses();
        last_bvh_analysis_update_ = GetTime();
    }
    
    void register_all_blas_for_analysis() {
        const auto& entries = blas_manager_->get_entries();
        
        for (const auto& entry : entries) {
            if (entry && entry->bvh && entry->mesh) {
                // Create a descriptive name for each BLAS
                std::string blas_name = "BLAS_" + std::to_string(entry->handle) + 
                                       " (" + std::to_string(entry->mesh->triCount) + " tris)";
                
                // Register BLAS with the BVH analyzer
                BVHReportManager::RegisterBVH(blas_name, entry->bvh.get(), entry->mesh.get());
                // Immediately update analysis for this BLAS
                BVHReportManager::UpdateAnalysis(blas_name);
            }
        }
    }
    
    void setup_rendering() {
        // Skip the expensive raytrace uber-shader compile for non-raytrace
        // captures (e.g. wireframe mesh debugging) so iterations are fast.
        const char* capMode = getenv("MSL_RENDER_MODE");
        bool skip_raytrace_shader = getenv("MSL_CAPTURE") && capMode && atoi(capMode) != 0;
        if (!skip_raytrace_shader) {
            raytracing_shader_ = LoadShaderCached("shaders/raytrace_tlas_blas_processed.fs");
            if (raytracing_shader_.id != 0) {
                static float s_materialTable[64 * MATERIAL_FLOATS_PER_DEF] = {0};
                MaterialRegistryPackForGPU(s_materialTable);
                int count = MaterialRegistryCount();
                int locTable = GetShaderLocation(raytracing_shader_, "materialTable");
                int locCount = GetShaderLocation(raytracing_shader_, "materialCount");
                // raylib uploads float arrays element-by-element via SHADER_UNIFORM_FLOAT count.
                SetShaderValueV(raytracing_shader_, locTable, s_materialTable,
                                SHADER_UNIFORM_FLOAT, count * MATERIAL_FLOATS_PER_DEF);
                SetShaderValue(raytracing_shader_, locCount, &count, SHADER_UNIFORM_INT);
            }
        }

        if (raytracing_shader_.id != 0) {
            setup_shader_uniforms();
        }
        
        camera_.position = {3.0f, 2.0f, 5.0f};
        camera_.target = {0.0f, 0.0f, 0.0f};
        camera_.up = {0.0f, 1.0f, 0.0f};
        camera_.fovy = 45.0f;
        camera_.projection = CAMERA_PERSPECTIVE;
    }

    
    
    void setup_shader_uniforms() {
        // Get camera and scene-level shader uniform locations
        camera_pos_loc_    = GetShaderLocation(raytracing_shader_, "cameraPos");
        camera_target_loc_ = GetShaderLocation(raytracing_shader_, "cameraTarget");
        camera_up_loc_     = GetShaderLocation(raytracing_shader_, "cameraUp");
        camera_fovy_loc_   = GetShaderLocation(raytracing_shader_, "cameraFovy");
        screen_size_loc_   = GetShaderLocation(raytracing_shader_, "screenSize");
        debug_triangle_tests_loc_ = GetShaderLocation(raytracing_shader_, "debugTriangleTests");
        
        // BLAS/TLAS uniforms are now handled by their respective managers
    }

    // Force the GPU driver to compile/link the raytrace program now, at startup,
    // instead of on the first frame the user toggles into raytrace mode. raylib's
    // LoadShader only does glLinkProgram; most drivers defer the expensive backend
    // codegen of this 1200+ line BVH-traversal uber-shader until the program is
    // first used in a draw. That first draw happens inside render_raytraced(), so
    // without this warm-up the whole single-threaded loop stalls for the entire
    // compile (seconds on a GPU, ~150s under llvmpipe) the first time raytracing
    // is enabled -- the window appears frozen and no input is processed. A trivial
    // 1x1 offscreen draw with the program bound triggers the same compile here,
    // where the startup logging already accounts for a brief delay.
    void warm_up_raytracing_shader() {
        if (raytracing_shader_.id == 0) return;
        printf("Warming up raytrace shader (one-time GPU compile)...\n");
        double t0 = GetTime();

        RenderTexture2D warm = LoadRenderTexture(1, 1);
        BeginTextureMode(warm);
        ClearBackground(BLACK);
        BeginShaderMode(raytracing_shader_);

        Vector2 screen_size = {1.0f, 1.0f};
        if (screen_size_loc_ != -1)
            SetShaderValue(raytracing_shader_, screen_size_loc_, &screen_size, SHADER_UNIFORM_VEC2);

        // Bind real BVH data so the texelFetch traversal paths are part of the
        // compiled program (also performs the initial texture upload early).
        blas_manager_->bind_to_shader(raytracing_shader_);
        tlas_manager_->bind_to_shader(raytracing_shader_, *blas_manager_);

        DrawRectangle(0, 0, 1, 1, WHITE);
        EndShaderMode();
        EndTextureMode();

        // Force raylib's batch to flush so the draw is issued to GL, then block on
        // glFinish so the driver actually compiles and executes the program now.
        // Without the glFinish the compile stays deferred until the first buffer
        // swap (in the real loop), which is exactly the stall we're moving here.
        rlDrawRenderBatchActive();
        glFinish();
        UnloadRenderTexture(warm);

        printf("Raytrace shader warm-up done in %.2fs\n", GetTime() - t0);
    }

    void update() {
        {
            PROFILE_SECTION("Input Handling");
            
            // Tab key toggles cursor mode (primary toggle for UI interaction)
            if (IsKeyPressed(KEY_TAB)) {
                cursor_disabled_ = !cursor_disabled_;
                if (cursor_disabled_) {
                    DisableCursor();
                    printf("Camera control mode: Mouse locked for camera movement\n");
                } else {
                    EnableCursor();
                    printf("UI interaction mode: Mouse unlocked for ImGui\n");
                }
            }
            
            // ESC key also toggles cursor (backup/legacy control)
            if (IsKeyPressed(KEY_ESCAPE)) {
                cursor_disabled_ = !cursor_disabled_;
                if (cursor_disabled_) {
                    DisableCursor();
                    printf("Camera control mode: Mouse locked for camera movement\n");
                } else {
                    EnableCursor();
                    printf("UI interaction mode: Mouse unlocked for ImGui\n");
                }
            }
            
            // Toggle rendering modes
            if (IsKeyPressed(KEY_R)) {
                render_mode_ = (render_mode_ + 1) % 4; // Cycle through 4 modes
                printf("Render mode: %s\n", 
                       render_mode_ == 0 ? "Ray Tracing" : 
                       render_mode_ == 1 ? "Surface Meshes" : 
                       render_mode_ == 2 ? "Wireframe Meshes" : "Debug BVH");
            }
            
            // BVH visualization toggle
            if (IsKeyPressed(KEY_B)) {
                show_bvh_visualization_ = !show_bvh_visualization_;
                printf("BVH visualization %s\n", show_bvh_visualization_ ? "enabled" : "disabled");
            }
            
            // Triangle test debug mode toggle
            if (IsKeyPressed(KEY_G)) {
                debug_triangle_tests_ = !debug_triangle_tests_;
                printf("Triangle test debug mode %s\n", debug_triangle_tests_ ? "enabled" : "disabled");
                printf("Green = few triangle tests, Yellow = moderate, Red = many tests per ray\n");
            }
            
            // Mesh visibility toggle
            if (IsKeyPressed(KEY_M)) {
                show_meshes_ = !show_meshes_;
                printf("Mesh visibility %s\n", show_meshes_ ? "enabled" : "disabled");
            }
            
            // Manual BLAS manager clear (for debugging)
            if (IsKeyPressed(KEY_C)) {
                printf("Manual BLAS manager clear requested\n");
                blas_manager_->clear();
                
                // Rebuild everything
                test_cluster_->rebuild_dirty_cells();
                printf("BLAS manager cleared and scene rebuilt\n");
            }
        }
        
        {
            PROFILE_SECTION("BVH Settings");
            // BVH visualization settings (only work when visualization is enabled)
            if (show_bvh_visualization_) {
                auto& settings = bvh_visualizer_->get_settings();
                
                if (IsKeyPressed(KEY_Q)) {
                    settings.show_blas_bvh = !settings.show_blas_bvh;
                    printf("BLAS BVH visualization %s\n", settings.show_blas_bvh ? "enabled" : "disabled");
                }
                if (IsKeyPressed(KEY_I)) {
                    settings.show_tlas_bvh = !settings.show_tlas_bvh;
                    printf("TLAS BVH visualization %s\n", settings.show_tlas_bvh ? "enabled" : "disabled");
                }
                if (IsKeyPressed(KEY_V)) {
                    settings.show_leaf_nodes = !settings.show_leaf_nodes;
                    printf("Leaf nodes %s\n", settings.show_leaf_nodes ? "enabled" : "disabled");
                }
                if (IsKeyPressed(KEY_T)) {
                    settings.show_interior_nodes = !settings.show_interior_nodes;
                    printf("Interior nodes %s\n", settings.show_interior_nodes ? "enabled" : "disabled");
                }
                if (IsKeyPressed(KEY_Y)) {
                    settings.use_depth_colors = !settings.use_depth_colors;
                    printf("Depth colors %s\n", settings.use_depth_colors ? "enabled" : "disabled");
                }
                if (IsKeyPressed(KEY_U)) {
                    settings.show_triangles = !settings.show_triangles;
                    printf("Triangle wireframes %s\n", settings.show_triangles ? "enabled" : "disabled");
                }
                if (IsKeyPressed(KEY_UP)) {
                    settings.max_depth_to_show = std::min(15, settings.max_depth_to_show + 1);
                    printf("Max depth to show: %d\n", settings.max_depth_to_show);
                }
                if (IsKeyPressed(KEY_DOWN)) {
                    settings.max_depth_to_show = std::max(1, settings.max_depth_to_show - 1);
                    printf("Max depth to show: %d\n", settings.max_depth_to_show);
                }
            }
        }
        
        {
            PROFILE_SECTION("Particle System Updates");
            // Add dynamic particle movement
            if (IsKeyPressed(KEY_SPACE)) {
                // Move some particles randomly to test dirty region updates
                for (int i = 0; i < 10; ++i) {
                    float x = (GetRandomValue(-50, 50) / 10.0f);
                    float y = (GetRandomValue(-50, 50) / 10.0f);
                    float z = (GetRandomValue(-50, 50) / 10.0f);
                    
                    Vector3 new_pos = {x, y, z};
                    // Use all 8 material types (0-7) for dynamic particles
                    uint32_t material = GetRandomValue(0, 7);
                    test_cluster_->add_particle(new_pos, 0.5f, material);
                }
                
                {
                    PROFILE_SECTION("Rebuild Dirty Cells");
                    test_cluster_->rebuild_dirty_cells();
                }
                printf("Added 10 random particles. Cluster now has %u cells\n", 
                       test_cluster_->get_cell_count());
            }
        }
        
        {
            PROFILE_SECTION("LOD Controls");
            // LOD level controls
            if (IsKeyPressed(KEY_ONE)) {
                printf("\n=== LOD CHANGE TO 0 ===\n");
                // printf("BEFORE: ");
                // blas_manager_->print_stats();
                
                test_cluster_->set_lod_level(0, true);  // clear_blas = true
                test_cluster_->rebuild_dirty_cells();
                
                // printf("AFTER REBUILD: ");
                // blas_manager_->print_stats();
                
                // printf("AFTER TLAS REBUILD: ");
                // blas_manager_->print_stats();
                // printf("========================\n");
            }
            if (IsKeyPressed(KEY_TWO)) {
                printf("\n=== LOD CHANGE TO 1 ===\n");
                // printf("BEFORE: ");
                // blas_manager_->print_stats();
                
                test_cluster_->set_lod_level(1, true);  // clear_blas = true
                test_cluster_->rebuild_dirty_cells();
                
                // printf("AFTER REBUILD: ");
                // blas_manager_->print_stats();
                
                // printf("AFTER TLAS REBUILD: ");
                // blas_manager_->print_stats();
                // printf("========================\n");
            }
            if (IsKeyPressed(KEY_THREE)) {
                printf("\n=== LOD CHANGE TO 2 ===\n");
                // printf("BEFORE: ");
                // blas_manager_->print_stats();
                
                test_cluster_->set_lod_level(2, true);  // clear_blas = true
                test_cluster_->rebuild_dirty_cells();
                
                // printf("AFTER REBUILD: ");
                // blas_manager_->print_stats();
                
                // printf("AFTER TLAS REBUILD: ");
                // blas_manager_->print_stats();
                // printf("========================\n");
            }
            if (IsKeyPressed(KEY_FOUR)) {
                printf("\n=== LOD CHANGE TO 3 ===\n");
                // printf("BEFORE: ");
                // blas_manager_->print_stats();
                
                test_cluster_->set_lod_level(3, true);  // clear_blas = true
                test_cluster_->rebuild_dirty_cells();
                
                // printf("AFTER REBUILD: ");
                // blas_manager_->print_stats();
                
                // printf("AFTER TLAS REBUILD: ");
                // blas_manager_->print_stats();
                // printf("========================\n");
            }
            if (IsKeyPressed(KEY_FIVE)) {
                printf("\n=== LOD CHANGE TO 4 ===\n");
                // printf("BEFORE: ");
                // blas_manager_->print_stats();
                
                test_cluster_->set_lod_level(4, true);  // clear_blas = true
                test_cluster_->rebuild_dirty_cells();
                
                // printf("AFTER REBUILD: ");
                // blas_manager_->print_stats();
                
                // printf("AFTER TLAS REBUILD: ");
                // blas_manager_->print_stats();
                // printf("========================\n");
            }
        }
        
        {
            PROFILE_SECTION("Camera Update");
            // Only update camera when cursor is disabled (camera control mode)
            if (cursor_disabled_) {
                UpdateCamera(&camera_, CAMERA_FREE);
            }
        }
    }
    
    
    void render() {
        // Start the Dear ImGui frame (skipped in headless capture mode)
        if (!capture_mode_) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
        }

        PROFILE_SECTION("BeginDrawing");
        BeginDrawing();
        ClearBackground(BLACK);
        
        if (render_mode_ == 0 && raytracing_shader_.id != 0) {
            PROFILE_SECTION("RayTracing Mode");
            render_raytraced();
        } else {
            PROFILE_SECTION("3D Rasterization Mode");
            
            {
                PROFILE_SECTION("BeginMode3D");
                BeginMode3D(camera_);
            }
            
            if (show_meshes_) {
                render_scene_meshes();
            }
            
            // Render BVH visualization if enabled or in debug mode
            if (show_bvh_visualization_ || render_mode_ == 3) {
                PROFILE_SECTION("BVH Visualization");
                
                // Configure visualization settings for selective rendering
                auto& settings = bvh_visualizer_->get_settings();
                
                // If a BVH is selected in the analysis window, only show that one
                if (!selected_bvh_for_analysis_.empty() && 
                    selected_bvh_for_analysis_ != "Main TLAS") {
                    // Strip " (BVH)" suffix if present to get clean name
                    std::string clean_name = selected_bvh_for_analysis_;
                    size_t suffix_pos = clean_name.find(" (BVH)");
                    if (suffix_pos != std::string::npos) {
                        clean_name = clean_name.substr(0, suffix_pos);
                    }
                    settings.selected_bvh_filter = clean_name;
                    settings.show_tlas_bvh = false;  // Hide TLAS when showing specific BLAS
                } else if (selected_bvh_for_analysis_ == "Main TLAS") {
                    settings.selected_bvh_filter = "";  // Show all BLAS
                    settings.show_blas_bvh = false;     // Hide BLAS when showing TLAS
                    settings.show_tlas_bvh = true;
                } else {
                    settings.selected_bvh_filter = "";  // Show all when nothing selected
                    settings.show_blas_bvh = true;
                    settings.show_tlas_bvh = true;
                }
                
                bvh_visualizer_->render(*blas_manager_, *tlas_manager_, settings);
            }
            
            {
                PROFILE_SECTION("Draw Grid");
                DrawGrid(20, 1.0f);
            }
            
            {
                PROFILE_SECTION("EndMode3D");
                EndMode3D();
            }
        }
        
        if (!capture_mode_) {
            PROFILE_SECTION("UI Rendering");
            render_ui();
        }

        // Flush raylib's batched geometry (e.g. the raytrace blit) to the framebuffer
        // before ImGui draws. ImGui's GL backend renders immediately, but raylib defers
        // its batch to EndDrawing — without this flush the full-screen raytrace quad is
        // drawn on top of the UI, hiding it in raytrace mode.
        rlDrawRenderBatchActive();

        // Render ImGui (skipped in headless capture mode)
        if (!capture_mode_) {
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        {
            PROFILE_SECTION("EndDrawing");
            EndDrawing();
        }
    }
    
    // (Re)create the offscreen target only when the required size changes. With dynamic
    // resolution scaling disabled the size is always the full window, so this allocates once
    // (and again only on a window resize) -- no per-frame FBO churn.
    void ensure_rt_target(int w, int h) {
        if (rt_target_.id != 0 && rt_w_ == w && rt_h_ == h) return;
        if (rt_target_.id != 0) UnloadRenderTexture(rt_target_);
        rt_target_ = LoadRenderTexture(w, h);
        SetTextureFilter(rt_target_.texture, TEXTURE_FILTER_BILINEAR);
        rt_w_ = w;
        rt_h_ = h;
    }

    void render_raytraced() {
        // Dynamic resolution scaling is disabled: always render the raytrace pass at full window
        // resolution. (Under vsync the scaler could not distinguish a too-heavy scale from
        // headroom, so it hunted up and down -- the visible resolution thrashing and FBO churn.)
        int full_w = GetScreenWidth();
        int full_h = GetScreenHeight();
        int rw = full_w;
        int rh = full_h;
        ensure_rt_target(rw, rh);

        BeginTextureMode(rt_target_);
        ClearBackground(BLACK);

        {
            PROFILE_SECTION("Shader Setup");
            BeginShaderMode(raytracing_shader_);

            // screenSize must match the offscreen target so ray generation is correct.
            Vector2 screen_size = {static_cast<float>(rw), static_cast<float>(rh)};

            SetShaderValue(raytracing_shader_, camera_pos_loc_, &camera_.position, SHADER_UNIFORM_VEC3);
            SetShaderValue(raytracing_shader_, camera_target_loc_, &camera_.target, SHADER_UNIFORM_VEC3);
            SetShaderValue(raytracing_shader_, camera_up_loc_, &camera_.up, SHADER_UNIFORM_VEC3);
            SetShaderValue(raytracing_shader_, camera_fovy_loc_, &camera_.fovy, SHADER_UNIFORM_FLOAT);
            SetShaderValue(raytracing_shader_, screen_size_loc_, &screen_size, SHADER_UNIFORM_VEC2);

            int debug_mode = debug_triangle_tests_ ? 1 : 0;
            SetShaderValue(raytracing_shader_, debug_triangle_tests_loc_, &debug_mode, SHADER_UNIFORM_INT);
        }

        {
            PROFILE_SECTION("BLAS Binding");
            blas_manager_->bind_to_shader(raytracing_shader_);
        }

        {
            PROFILE_SECTION("TLAS Binding");
            tlas_manager_->bind_to_shader(raytracing_shader_, *blas_manager_);
        }

        {
            PROFILE_SECTION("Fullscreen Quad");
            DrawRectangle(0, 0, rw, rh, WHITE);
        }

        {
            PROFILE_SECTION("End Shader");
            EndShaderMode();
        }

        EndTextureMode();

        // Blit the offscreen result upscaled to the screen (negative source height flips Y).
        DrawTexturePro(rt_target_.texture,
                       (Rectangle){0.0f, 0.0f, static_cast<float>(rw), -static_cast<float>(rh)},
                       (Rectangle){0.0f, 0.0f, static_cast<float>(full_w), static_cast<float>(full_h)},
                       (Vector2){0.0f, 0.0f}, 0.0f, WHITE);
    }
    
    void render_scene_meshes() {
        const auto& draw_records = [this]() {
            PROFILE_SECTION("Get Draw Records");
            return tlas_manager_->get_draw_records();
        }();
        
        PROFILE_SECTION("Mesh Rendering Loop");
        int triangles_rendered = 0;
        int meshes_rendered = 0;
        
        for (size_t i = 0; i < draw_records.size(); i++) {
            const auto& record = draw_records[i];
            
            // Get the mesh for this BLAS handle
            auto* mesh = blas_manager_->get_mesh(record.blas_handle);
            if (!mesh || !mesh->tri || mesh->triCount == 0) {
                continue;
            }
            
            meshes_rendered++;
            triangles_rendered += mesh->triCount;
            
            // Choose color based on material ID and instance
            Color mesh_colors[] = {GREEN, BLUE, RED, YELLOW, PURPLE, ORANGE};
            Color mesh_color = mesh_colors[(record.material_id + record.instance_id) % 6];
            
            // Apply transform matrix
            rlPushMatrix();
            
            // Convert Matrix4x4 to OpenGL matrix format (column-major)
            const auto& m = record.transform.m;
            float gl_matrix[16] = {
                m[0], m[4], m[8],  m[12],
                m[1], m[5], m[9],  m[13],
                m[2], m[6], m[10], m[14],
                m[3], m[7], m[11], m[15]
            };
            rlMultMatrixf(gl_matrix);
            
            // Determine transparency based on render mode
            unsigned char alpha = (render_mode_ == 3) ? 80 : 120; // More transparent in debug mode
            
            // Render mesh as filled triangles with transparency
            rlBegin(RL_TRIANGLES);
            rlColor4ub(mesh_color.r, mesh_color.g, mesh_color.b, alpha);
            
            for (int tri_idx = 0; tri_idx < mesh->triCount; tri_idx++) {
                const auto& tri = mesh->tri[tri_idx];
                
                // Vertex 0
                rlVertex3f(tri.vertex0.x, tri.vertex0.y, tri.vertex0.z);
                // Vertex 1  
                rlVertex3f(tri.vertex1.x, tri.vertex1.y, tri.vertex1.z);
                // Vertex 2
                rlVertex3f(tri.vertex2.x, tri.vertex2.y, tri.vertex2.z);
            }
            rlEnd();
            
            // Also draw wireframe edges for better definition
            rlBegin(RL_LINES);
            rlColor4ub(mesh_color.r, mesh_color.g, mesh_color.b, 255); // Full opacity for edges
            
            for (int tri_idx = 0; tri_idx < mesh->triCount; tri_idx++) {
                const auto& tri = mesh->tri[tri_idx];
                
                // Edge 0-1
                rlVertex3f(tri.vertex0.x, tri.vertex0.y, tri.vertex0.z);
                rlVertex3f(tri.vertex1.x, tri.vertex1.y, tri.vertex1.z);
                
                // Edge 1-2
                rlVertex3f(tri.vertex1.x, tri.vertex1.y, tri.vertex1.z);
                rlVertex3f(tri.vertex2.x, tri.vertex2.y, tri.vertex2.z);
                
                // Edge 2-0
                rlVertex3f(tri.vertex2.x, tri.vertex2.y, tri.vertex2.z);
                rlVertex3f(tri.vertex0.x, tri.vertex0.y, tri.vertex0.z);
            }
            rlEnd();
            
            rlPopMatrix();
        }
        
        // Store stats for reporting
        last_triangles_rendered_ = triangles_rendered;
        last_meshes_rendered_ = meshes_rendered;
    }
    
    
    void render_ui() {
        // Main control panel - positioned on the left
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(350, 500), ImGuiCond_FirstUseEver);
        ImGui::Begin("MatterSurfaceLib Controls");
        
        // Performance info
        double fps = 1000.0 / Performance::Profiler::instance().get_frame_time_ms();
        ImGui::Text("FPS: %.1f (%.2f ms)", fps, Performance::Profiler::instance().get_frame_time_ms());
        
        // Cursor mode indicator
        if (cursor_disabled_) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Mode: Camera Control (TAB to unlock)");
        } else {
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Mode: UI Interaction (TAB to lock)");
        }
        
        ImGui::Separator();
        
        // Render mode selection
        const char* render_modes[] = {"Ray Tracing", "Surface Meshes", "Wireframe Meshes", "Debug BVH"};
        if (ImGui::Combo("Render Mode", &render_mode_, render_modes, 4)) {
            printf("Render mode changed to: %s\n", render_modes[render_mode_]);
        }
        
        ImGui::Separator();

        // Mesh simplification ratio: 1.0 = full detail, lower = cheaper proxy.
        // Rebuilds all cells through the simplifier when changed.
        {
            float ratio = test_cluster_->get_simplification_ratio();
            if (ImGui::SliderFloat("Simplification", &ratio, 0.05f, 1.0f, "%.2f")) {
                test_cluster_->set_simplification_ratio(ratio);
                test_cluster_->force_rebuild_all_cells();
            }
        }

        ImGui::Separator();

        // Camera controls — clickable orbit/zoom so the view is fully navigable without
        // locking the cursor or using WASD (important over remote desktop). Buttons use
        // auto-repeat so holding them down moves the camera continuously.
        ImGui::Text("Camera");
        {
            float dx = camera_.position.x - camera_.target.x;
            float dy = camera_.position.y - camera_.target.y;
            float dz = camera_.position.z - camera_.target.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < 0.0001f) dist = 0.0001f;
            float yaw = atan2f(dz, dx);
            float pitch = asinf(dy / dist);
            bool changed = false;
            const float orbit_step = 0.04f; // radians per repeat tick

            ImGui::PushButtonRepeat(true);
            ImGui::Text("Orbit:");
            if (ImGui::Button("Left"))  { yaw -= orbit_step; changed = true; }
            ImGui::SameLine();
            if (ImGui::Button("Right")) { yaw += orbit_step; changed = true; }
            ImGui::SameLine();
            if (ImGui::Button("Up"))    { pitch += orbit_step; changed = true; }
            ImGui::SameLine();
            if (ImGui::Button("Down"))  { pitch -= orbit_step; changed = true; }

            if (ImGui::Button("Zoom In"))  { dist *= 0.96f; changed = true; }
            ImGui::SameLine();
            if (ImGui::Button("Zoom Out")) { dist *= 1.04f; changed = true; }
            ImGui::PopButtonRepeat();

            if (ImGui::SliderFloat("Distance", &dist, 1.0f, 150.0f)) changed = true;

            // Clamp pitch just shy of the poles so the orbit never flips/gimbal-locks.
            const float pitch_limit = 1.5533f; // ~89 degrees
            if (pitch > pitch_limit) pitch = pitch_limit;
            if (pitch < -pitch_limit) pitch = -pitch_limit;
            if (dist < 1.0f) dist = 1.0f;

            if (changed) {
                camera_.position.x = camera_.target.x + dist * cosf(pitch) * cosf(yaw);
                camera_.position.y = camera_.target.y + dist * sinf(pitch);
                camera_.position.z = camera_.target.z + dist * cosf(pitch) * sinf(yaw);
            }

            if (ImGui::Button("Reset View")) {
                camera_.position = {3.0f, 2.0f, 5.0f};
                camera_.target = {0.0f, 0.0f, 0.0f};
                camera_.up = {0.0f, 1.0f, 0.0f};
            }
        }

        ImGui::Separator();

        // Particle system controls
        ImGui::Text("Particle System");
        if (ImGui::Button("Add Random Particles")) {
            // Add 10 random particles (same as SPACE key)
            for (int i = 0; i < 10; ++i) {
                float x = (GetRandomValue(-50, 50) / 10.0f);
                float y = (GetRandomValue(-50, 50) / 10.0f);
                float z = (GetRandomValue(-50, 50) / 10.0f);
                
                Vector3 new_pos = {x, y, z};
                uint32_t material = GetRandomValue(0, 7);
                test_cluster_->add_particle(new_pos, 0.5f, material);
            }
            test_cluster_->rebuild_dirty_cells();
        }
        
        ImGui::Text("Particles: %u, Cells: %u", 
                   test_cluster_->get_particle_count(),
                   test_cluster_->get_cell_count());
        
        // LOD controls
        int current_lod = test_cluster_->get_lod_level();
        if (ImGui::SliderInt("LOD Level", &current_lod, 0, 4)) {
            test_cluster_->set_lod_level(current_lod, true);
            test_cluster_->rebuild_dirty_cells();
        }
        ImGui::Text("Cell Size: %.2f units", test_cluster_->get_current_cell_size());
        
        ImGui::Separator();
        
        // Visualization controls
        ImGui::Text("Visualization");
        ImGui::Checkbox("Show Meshes", &show_meshes_);
        ImGui::Checkbox("BVH Visualization", &show_bvh_visualization_);
        ImGui::Checkbox("Debug Triangle Tests", &debug_triangle_tests_);
        
        // BVH settings (only when visualization is enabled)
        if (show_bvh_visualization_ || render_mode_ == 3) {
            auto& settings = bvh_visualizer_->get_settings();
            ImGui::Text("BVH Settings:");
            ImGui::Checkbox("Show BLAS BVH", &settings.show_blas_bvh);
            ImGui::Checkbox("Show TLAS BVH", &settings.show_tlas_bvh);
            ImGui::Checkbox("Show Leaf Nodes", &settings.show_leaf_nodes);
            ImGui::Checkbox("Show Interior Nodes", &settings.show_interior_nodes);
            ImGui::Checkbox("Use Depth Colors", &settings.use_depth_colors);
            ImGui::Checkbox("Show Triangles", &settings.show_triangles);
            ImGui::SliderInt("Max Depth", &settings.max_depth_to_show, 1, 15);
        }
        
        ImGui::Separator();
        
        // System statistics
        ImGui::Text("System Statistics");
        ImGui::Text("BLAS Entries: %d", blas_manager_->get_unique_blas_count());
        ImGui::Text("Total Triangles: %d", blas_manager_->get_total_triangle_count());
        
        if (render_mode_ != 0) {
            ImGui::Text("Rendered Meshes: %d", last_meshes_rendered_);
            ImGui::Text("Rendered Triangles: %d", last_triangles_rendered_);
        }
        
        if (ImGui::Button("Clear BLAS Manager")) {
            blas_manager_->clear();
            test_cluster_->rebuild_dirty_cells();
            printf("BLAS manager cleared and scene rebuilt\n");
        }
        
        ImGui::End();
        
        // Material reference window - positioned bottom left
        ImGui::SetNextWindowPos(ImVec2(20, 540), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(350, 180), ImGuiCond_FirstUseEver);
        ImGui::Begin("Material Reference");
        ImGui::Text("Material Types:");
        ImGui::BulletText("0: Red metallic");
        ImGui::BulletText("1: Blue diffuse");
        ImGui::BulletText("2: Green ground");
        ImGui::BulletText("3: Gold metallic");
        ImGui::BulletText("4: Clear glass");
        ImGui::BulletText("5: Emissive light");
        ImGui::BulletText("6: Green glass");
        ImGui::BulletText("7: Water");
        ImGui::End();
        
        // Keyboard shortcuts help - positioned center bottom
        ImGui::SetNextWindowPos(ImVec2(390, 540), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(400, 180), ImGuiCond_FirstUseEver);
        ImGui::Begin("Keyboard Shortcuts");
        ImGui::Text("Controls:");
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "TAB: Toggle cursor mode (UI/Camera)");
        ImGui::BulletText("ESC: Toggle cursor (backup)");
        ImGui::BulletText("SPACE: Add random particles");
        ImGui::BulletText("R: Cycle render modes");
        ImGui::BulletText("B: Toggle BVH visualization");
        ImGui::BulletText("G: Toggle triangle test debug");
        ImGui::BulletText("M: Toggle mesh visibility");
        ImGui::BulletText("C: Clear BLAS manager");
        ImGui::BulletText("1-5: Change LOD level");
        if (show_bvh_visualization_) {
            ImGui::Text("BVH Controls:");
            ImGui::BulletText("Q: Toggle BLAS BVH");
            ImGui::BulletText("I: Toggle TLAS BVH");
            ImGui::BulletText("V: Toggle leaf nodes");
            ImGui::BulletText("T: Toggle interior nodes");
            ImGui::BulletText("Y: Toggle depth colors");
            ImGui::BulletText("U: Toggle triangles");
            ImGui::BulletText("UP/DOWN: Adjust max depth");
        }
        ImGui::End();
        
        // BVH Analysis Window - positioned on the right side
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 400, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380, 600), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("BVH Analysis")) {
            ImGui::Checkbox("Show BVH Analysis", &show_bvh_analysis_window_);
            ImGui::Checkbox("Auto Update", &auto_update_bvh_analysis_);
            
            if (ImGui::Button("Manual Update All")) {
                BVHReportManager::UpdateAllAnalyses();
                last_bvh_analysis_update_ = GetTime();
            }
            
            ImGui::SameLine();
            if (ImGui::Button("Clear All")) {
                BVHReportManager::Clear();
            }
            
            ImGui::Text("Last Update: %.2f seconds ago", GetTime() - last_bvh_analysis_update_);
            
            // Auto-update if enabled and enough time has passed
            if (auto_update_bvh_analysis_ && (GetTime() - last_bvh_analysis_update_) > 2.0f) {
                BVHReportManager::UpdateAllAnalyses();
                last_bvh_analysis_update_ = GetTime();
            }
            
            ImGui::Separator();
            
            // List of registered BVH structures
            auto registered_names = BVHReportManager::GetRegisteredNames();
            if (!registered_names.empty()) {
                ImGui::Text("Registered BVH Structures:");
                for (const auto& name : registered_names) {
                    if (ImGui::Selectable(name.c_str(), selected_bvh_for_analysis_ == name)) {
                        selected_bvh_for_analysis_ = name;
                    }
                }
                
                ImGui::Separator();
                
                // Show quick stats for TLAS
                const TLASAnalysis* tlas_analysis = BVHReportManager::GetTLASAnalysis("Main TLAS");
                if (tlas_analysis) {
                    ImGui::Text("TLAS Quick Stats:");
                    ImGui::Text("Quality Score: %.1f/100", tlas_analysis->tlas_quality_score);
                    ImGui::Text("Instances: %u", tlas_analysis->total_instances);
                    ImGui::Text("TLAS Nodes: %u", tlas_analysis->tlas_nodes);
                    ImGui::Text("Max Depth: %u", tlas_analysis->max_tlas_depth);
                    ImGui::Text("Balance Factor: %.3f", tlas_analysis->tlas_balance_factor);
                    
                    // Color-code quality
                    if (tlas_analysis->tlas_quality_score >= 80) {
                        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Status: EXCELLENT");
                    } else if (tlas_analysis->tlas_quality_score >= 60) {
                        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Status: GOOD");
                    } else if (tlas_analysis->tlas_quality_score >= 40) {
                        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Status: FAIR");
                    } else {
                        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Status: POOR");
                    }
                }
                
                ImGui::Separator();
                
                // BLAS statistics summary
                ImGui::Text("BLAS Manager Stats:");
                ImGui::Text("Active BLAS: %d", blas_manager_->get_unique_blas_count());
                ImGui::Text("Total Triangles: %d", blas_manager_->get_total_triangle_count());
                
                // Register any new BLAS for analysis
                if (ImGui::Button("Register Current BLAS")) {
                    // This would need access to individual BLAS structures
                    // For now, we'll focus on TLAS analysis
                    ImGui::Text("(BLAS registration needs individual mesh access)");
                }
                
            } else {
                ImGui::Text("No BVH structures registered.");
                ImGui::Text("Analysis will begin after scene setup.");
            }
        }
        ImGui::End();
        
        // Detailed BVH Analysis Window (popup) - positioned center-right
        if (show_bvh_analysis_window_) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 650, 100), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(620, 700), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Detailed BVH Analysis", &show_bvh_analysis_window_)) {
                
                if (!selected_bvh_for_analysis_.empty()) {
                    ImGui::Text("Analysis for: %s", selected_bvh_for_analysis_.c_str());
                    ImGui::Separator();
                    
                    // Strip the " (BVH)" or " (TLAS)" suffix that GetRegisteredNames() adds
                    std::string clean_name = selected_bvh_for_analysis_;
                    size_t suffix_pos = clean_name.find(" (BVH)");
                    if (suffix_pos != std::string::npos) {
                        clean_name = clean_name.substr(0, suffix_pos);
                    }
                    suffix_pos = clean_name.find(" (TLAS)");
                    if (suffix_pos != std::string::npos) {
                        clean_name = clean_name.substr(0, suffix_pos);
                    }
                    
                    // Show detailed TLAS analysis
                    const TLASAnalysis* tlas_analysis = BVHReportManager::GetTLASAnalysis(clean_name);
                    if (tlas_analysis && selected_bvh_for_analysis_.find("TLAS") != std::string::npos) {
                        
                        ImGui::Text("=== TLAS DETAILED ANALYSIS ===");
                        
                        // Quality metrics
                        ImGui::Text("Overall Quality Score: %.2f/100", tlas_analysis->tlas_quality_score);
                        ImGui::ProgressBar(tlas_analysis->tlas_quality_score / 100.0f);
                        
                        // Structure metrics
                        ImGui::Text("Structure Metrics:");
                        ImGui::Indent();
                        ImGui::Text("Total Instances: %u", tlas_analysis->total_instances);
                        ImGui::Text("TLAS Nodes: %u", tlas_analysis->tlas_nodes);
                        ImGui::Text("Max TLAS Depth: %u", tlas_analysis->max_tlas_depth);
                        ImGui::Text("Balance Factor: %.3f", tlas_analysis->tlas_balance_factor);
                        ImGui::Text("Surface Area: %.2f", tlas_analysis->tlas_surface_area);
                        ImGui::Unindent();
                        
                        // Performance metrics
                        ImGui::Text("Performance Metrics:");
                        ImGui::Indent();
                        ImGui::Text("Avg Instance Triangles: %.1f", tlas_analysis->avg_instance_triangles);
                        ImGui::Text("Instance Distribution Variance: %.2f", tlas_analysis->instance_distribution_variance);
                        ImGui::Text("Analysis Time: %.3f ms", tlas_analysis->total_analysis_time_ms);
                        ImGui::Unindent();
                        
                        // Issues and recommendations
                        if (!tlas_analysis->tlas_issues.empty()) {
                            ImGui::Text("Issues:");
                            ImGui::Indent();
                            for (const auto& issue : tlas_analysis->tlas_issues) {
                                ImGui::BulletText("%s", issue.c_str());
                            }
                            ImGui::Unindent();
                        }
                        
                        if (!tlas_analysis->tlas_recommendations.empty()) {
                            ImGui::Text("Recommendations:");
                            ImGui::Indent();
                            for (const auto& rec : tlas_analysis->tlas_recommendations) {
                                ImGui::BulletText("%s", rec.c_str());
                            }
                            ImGui::Unindent();
                        }
                        
                        // Generate text report button
                        if (ImGui::Button("Generate Full Text Report")) {
                            std::string report = BVHReportManager::GenerateFullReport();
                            printf("%s", report.c_str());
                        }
                    }
                    
                    // Show detailed BLAS analysis (for non-TLAS structures)
                    const BVHTreeAnalysis* bvh_analysis = BVHReportManager::GetBVHAnalysis(clean_name);
                    
                    bool is_not_tlas = selected_bvh_for_analysis_.find("TLAS") == std::string::npos;
                    
                    if (bvh_analysis && is_not_tlas) {
                        ImGui::Text("=== BLAS DETAILED ANALYSIS ===");
                        
                        // Quality metrics
                        ImGui::Text("Overall Quality Score: %.2f/100", bvh_analysis->overall_quality_score);
                        ImGui::ProgressBar(bvh_analysis->overall_quality_score / 100.0f);
                        
                        // Structure metrics
                        ImGui::Text("Structure Metrics:");
                        ImGui::Indent();
                        ImGui::Text("Total Nodes: %u", bvh_analysis->total_nodes);
                        ImGui::Text("Leaf Nodes: %u", bvh_analysis->leaf_nodes);
                        ImGui::Text("Internal Nodes: %u", bvh_analysis->internal_nodes);
                        ImGui::Text("Total Triangles: %u", bvh_analysis->total_triangles);
                        ImGui::Text("Max Depth: %u", bvh_analysis->max_depth);
                        ImGui::Text("Min Depth: %u", bvh_analysis->min_depth);
                        ImGui::Text("Avg Depth: %.2f", bvh_analysis->avg_depth);
                        ImGui::Text("Balance Factor: %.3f", bvh_analysis->balance_factor);
                        ImGui::Text("Tree Efficiency: %.3f", bvh_analysis->tree_efficiency);
                        ImGui::Text("Node Utilization: %.3f", bvh_analysis->node_utilization);
                        ImGui::Unindent();
                        
                        // Triangle distribution
                        ImGui::Text("Triangle Distribution:");
                        ImGui::Indent();
                        ImGui::Text("Max Triangles per Leaf: %u", bvh_analysis->max_triangles_per_leaf);
                        ImGui::Text("Min Triangles per Leaf: %u", bvh_analysis->min_triangles_per_leaf);
                        ImGui::Text("Avg Triangles per Leaf: %.2f", bvh_analysis->avg_triangles_per_leaf);
                        ImGui::Text("Triangle Variance: %.2f", bvh_analysis->triangle_distribution_variance);
                        ImGui::Unindent();
                        
                        // Performance metrics
                        ImGui::Text("Performance Metrics:");
                        ImGui::Indent();
                        ImGui::Text("Surface Area: %.2f", bvh_analysis->total_surface_area);
                        ImGui::Text("Avg Node Surface Area: %.2f", bvh_analysis->avg_node_surface_area);
                        ImGui::Text("Surface Area Ratio: %.3f", bvh_analysis->surface_area_ratio);
                        ImGui::Text("Estimated Traversal Cost: %.2f", bvh_analysis->estimated_traversal_cost);
                        ImGui::Text("Memory Usage: %u bytes", bvh_analysis->memory_usage_bytes);
                        ImGui::Text("Memory Efficiency: %.3f", bvh_analysis->memory_efficiency);
                        ImGui::Text("Analysis Time: %.3f ms", bvh_analysis->analysis_time_ms);
                        ImGui::Unindent();
                        
                        // Issues and recommendations
                        if (!bvh_analysis->quality_issues.empty()) {
                            ImGui::Text("Quality Issues:");
                            ImGui::Indent();
                            for (const auto& issue : bvh_analysis->quality_issues) {
                                ImGui::BulletText("%s", issue.c_str());
                            }
                            ImGui::Unindent();
                        }
                        
                        if (!bvh_analysis->recommendations.empty()) {
                            ImGui::Text("Recommendations:");
                            ImGui::Indent();
                            for (const auto& rec : bvh_analysis->recommendations) {
                                ImGui::BulletText("%s", rec.c_str());
                            }
                            ImGui::Unindent();
                        }
                        
                        // Depth distribution chart
                        if (!bvh_analysis->nodes_per_depth.empty()) {
                            ImGui::Text("Depth Distribution:");
                            ImGui::Indent();
                            for (size_t depth = 0; depth < bvh_analysis->nodes_per_depth.size(); depth++) {
                                if (bvh_analysis->nodes_per_depth[depth] > 0) {
                                    ImGui::Text("Depth %zu: %u nodes, %u triangles", 
                                               depth, bvh_analysis->nodes_per_depth[depth],
                                               depth < bvh_analysis->triangles_per_depth.size() ? 
                                               bvh_analysis->triangles_per_depth[depth] : 0);
                                }
                            }
                            ImGui::Unindent();
                        }
                    }
                    
                } else {
                    ImGui::Text("Select a BVH structure from the main analysis window.");
                }
            }
            ImGui::End();
        }
    }
    
    void print_rendering_stats() {
        printf("\n=== RENDERING SYSTEM STATISTICS ===\n");
        
        // BLAS stats
        printf("BLAS Manager:\n");
        printf("  - Active BLAS count: %d\n", blas_manager_->get_unique_blas_count());
        
        size_t total_triangles = 0;
        size_t total_vertices = 0;
        int blas_count = blas_manager_->get_unique_blas_count();
        for (int i = 0; i < blas_count; i++) {
            auto* mesh = blas_manager_->get_mesh(i);
            if (mesh) {
                total_triangles += mesh->triCount;
                total_vertices += mesh->triCount * 3; // Approximate vertex count
            }
        }
        printf("  - Total triangles: %zu\n", total_triangles);
        printf("  - Total vertices: %zu\n", total_vertices);
        printf("  - Memory usage: ~%.2f MB\n", (total_triangles * sizeof(Tri) + total_vertices * sizeof(Vector3)) / (1024.0 * 1024.0));
        
        // TLAS stats
        printf("TLAS Manager:\n");
        const auto& draw_records = tlas_manager_->get_draw_records();
        printf("  - Draw records: %zu\n", draw_records.size());
        printf("  - Active instances: %zu\n", draw_records.size());
        
        // Cluster stats
        printf("Cluster System:\n");
        printf("  - Particles: %u\n", test_cluster_->get_particle_count());
        printf("  - Cells: %u\n", test_cluster_->get_cell_count());
        printf("  - Dirty cells: %u\n", test_cluster_->get_dirty_cell_count());
        printf("  - LOD level: %d (%.2f unit cells)\n", 
               test_cluster_->get_lod_level(), test_cluster_->get_current_cell_size());
        
        // Shader stats
        printf("Shader System:\n");
        printf("  - Ray tracing shader loaded: %s\n", raytracing_shader_.id != 0 ? "YES" : "NO");
        if (raytracing_shader_.id != 0) {
            printf("  - Shader ID: %u\n", raytracing_shader_.id);
        }
        
        printf("========================================\n");
    }
    


    void cleanup() {
        if (raytracing_shader_.id != 0) UnloadShader(raytracing_shader_);
        if (rt_target_.id != 0) UnloadRenderTexture(rt_target_);
        // Managers clean up their own textures in destructors
    }
    
private:
    int screen_width_;
    int screen_height_;
    
    std::unique_ptr<BLASManager> blas_manager_;
    std::unique_ptr<TLASManager> tlas_manager_;
    std::unique_ptr<BVHVisualizer> bvh_visualizer_;
    std::unique_ptr<Cluster> test_cluster_;
    
    // Scene geometry BLAS handles
    BLASHandle sphere_blas_;
    BLASHandle ground_blas_;
    
    // Mapping between BVH analysis names and BLAS handles for selective rendering
    std::unordered_map<std::string, BLASHandle> bvh_name_to_handle_;
    
    Camera camera_;
    Shader raytracing_shader_{};

    // Offscreen target for the raytrace pass, rendered at full window resolution.
    RenderTexture2D rt_target_{};
    int rt_w_ = 0, rt_h_ = 0;

    bool cursor_disabled_ = false;
    int render_mode_ = 3; // 0=raytracing, 1=solid_meshes, 2=wireframe_meshes, 3=debug_bvh
    bool show_bvh_visualization_ = false;
    bool show_meshes_ = true;
    
    int camera_pos_loc_;
    int camera_target_loc_;
    int camera_up_loc_;
    int camera_fovy_loc_;
    int screen_size_loc_;
    int debug_triangle_tests_loc_;
    
    // Debug modes
    bool debug_triangle_tests_ = false;
    
    // Performance tracking
    int last_triangles_rendered_ = 0;
    int last_meshes_rendered_ = 0;
    
    // BVH Analysis
    bool show_bvh_analysis_window_ = false;
    bool auto_update_bvh_analysis_ = true;
    std::string selected_bvh_for_analysis_;
    float last_bvh_analysis_update_ = 0.0f;

};

int main() {
    // Enable Mesa's on-disk shader cache before any GL context is created.
    // Under WSLg the d3d12 GL driver otherwise re-translates every shader on
    // each launch. Non-overriding (overwrite=0) so an explicit user env wins.
    // This complements our own program-binary cache: it also speeds up the
    // default raylib / ImGui shaders, not just the raytrace uber-shader.
    // Mesa-only; the env is irrelevant to the native Windows NVIDIA driver.
#ifndef _WIN32
    setenv("MESA_SHADER_CACHE_DISABLE", "false", 0);
#endif

    try {
        MatterSurfaceLibDemo demo(1280, 800);
        demo.run();
    } catch (const std::exception& e) {
        printf("Error: %s\n", e.what());
        return 1;
    }
    
    return 0;
}