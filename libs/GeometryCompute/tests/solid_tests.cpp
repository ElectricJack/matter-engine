#include <matter/geometry_compute/solid.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#ifdef MATTER_GEOMETRY_COMPUTE
#include <matter/compute/vulkan.h>
#endif
namespace gc = matter::geometry_compute;
namespace cd = matter::compute;
namespace ms = matter::services;
namespace mc = matter::contracts;
namespace {
void check(bool ok, const char *fixture) {
    if (!ok) { std::cerr << "FAIL " << fixture << '\n'; std::exit(1); }
}
void valid(const ms::AssetResult &r, const char *fixture) {
    if (!r.ok()) for (const auto &d : r.diagnostics()) std::cerr << mc::code_name(d.code) << ": " << d.message << '\n';
    check(r.ok(),fixture);
}
void fails(const ms::AssetResult &r, mc::ErrorCode code, const char *fixture) {
    check(!r.ok() && !r.value() && !r.diagnostics().empty() && r.diagnostics()[0].code == code,fixture);
}
const ms::JobEnvelope job{"compute.fixture","revision.1",3};
gc::SolidRequest sphere(cd::DeviceId device) {
    gc::SolidRequest r; r.device = device; r.spacing_metres = .25f;
    r.source = {{"fixture"},"fixture://solid","1",{}, {}};
    r.required_capabilities = {{"matter.geometry.solid.compute",1}};
    gc::SolidElement s; s.shape = gc::Shape::sphere; r.elements.push_back(s);
    return r;
}
class Fake final : public cd::Device {
public:
    enum Mode { failure, malformed, stale, cancel } mode = failure;
    unsigned calls = 0;
    ms::CancellationSource cancellation;
    cd::Result execute(const ms::JobEnvelope &e, const cd::Request &, ms::JobMonitor &) override {
        ++calls;
        if (mode == failure) return cd::Result::failure(e,mc::ErrorCode::backend_failure,"fixture failure");
        if (mode == cancel) cancellation.cancel();
        auto response = e; if (mode == stale) ++response.generation_epoch;
        return cd::Result::success(response,{});
    }
};
#ifdef MATTER_GEOMETRY_COMPUTE
void memory_only_validation() {
    // Keep shader/synchronization checks, but never reuse a disk validation
    // result. Cover both older and current validation-layer setting names.
    const auto setting = [](const char *name, const char *value) {
#ifdef _WIN32
        check(_putenv_s(name,value) == 0,"GPU-NO-DISK-CACHE-SETTING");
#else
        check(setenv(name,value,1) == 0,"GPU-NO-DISK-CACHE-SETTING");
#endif
    };
    setting("VK_LAYER_DISABLES","VK_VALIDATION_FEATURE_DISABLE_SHADER_VALIDATION_CACHE_EXT");
    setting("VK_LAYER_CHECK_SHADERS_CACHING","0");
    setting("MESA_SHADER_CACHE_DISABLE","true");
}
void admission() {
    auto fake = std::make_shared<Fake>(); gc::SolidBackend backend(fake);
    auto r = sphere(fake->id()); auto options = ms::EvaluationOptions{};
    auto broken = r; broken.required_capabilities.push_back({"unknown.compute",1});
    fails(backend.generate(job,broken),mc::ErrorCode::unsupported_capability,"GPU-CAPS");
    broken = r; broken.required_capabilities[0].version = 2;
    fails(backend.generate(job,broken),mc::ErrorCode::unsupported_capability,"GPU-CAPS-VERSION");
    broken = r; ++broken.device;
    fails(backend.generate(job,broken),mc::ErrorCode::invalid_input,"GPU-DEVICE-MISMATCH");
    options.budget.operations = 1;
    fails(backend.generate(job,r,options),mc::ErrorCode::resource_limit,"GPU-OPERATIONS");
    options = {}; options.budget.working_bytes = 1;
    fails(backend.generate(job,r,options),mc::ErrorCode::resource_limit,"GPU-MEMORY");
    options = {}; options.budget.output_bytes = 1;
    fails(backend.generate(job,r,options),mc::ErrorCode::resource_limit,"GPU-OUTPUT");
    broken = r; broken.max_grid_vertices = 1;
    fails(backend.generate(job,broken),mc::ErrorCode::resource_limit,"GPU-GRID");
    broken = r; broken.elements.resize(257);
    fails(backend.generate(job,broken),mc::ErrorCode::resource_limit,"GPU-TAPE-LIMIT");
    broken = r; broken.spacing_metres = std::numeric_limits<float>::infinity();
    fails(backend.generate(job,broken),mc::ErrorCode::invalid_input,"GPU-NONFINITE");
    broken = r; broken.elements[0].local_to_world(0,0) = 2;
    fails(backend.generate(job,broken),mc::ErrorCode::unsupported_capability,"GPU-AFFINE-UNSUPPORTED");
    broken = r; broken.elements[0].local_to_world(0,0) = -1;
    fails(backend.generate(job,broken),mc::ErrorCode::unsupported_capability,"GPU-MIRROR-UNSUPPORTED");
    ms::CancellationSource cancelled; cancelled.cancel();
    fails(backend.generate(job,r,{}, {cancelled.token(),{}}),mc::ErrorCode::cancelled,"GPU-CANCEL-BEFORE");
    check(fake->calls == 0,"GPU-REJECT-BEFORE-DISPATCH");
    fails(backend.generate(job,r),mc::ErrorCode::backend_failure,"GPU-FAILURE");
    fake->mode = Fake::malformed;
    fails(backend.generate(job,r),mc::ErrorCode::backend_failure,"GPU-MALFORMED-READBACK");
    fake->mode = Fake::stale;
    fails(backend.generate(job,r),mc::ErrorCode::stale_result,"GPU-STALE");
    fake->mode = Fake::cancel;
    fails(backend.generate(job,r,{}, {fake->cancellation.token(),{}}),mc::ErrorCode::cancelled,"GPU-CANCEL-AFTER-DISPATCH");
    std::cout << "PASS GPU admission/cancellation/malformed/identity fixtures\n";
}
void gpu() {
    mc::ValidationResult created;
    auto device = cd::create_vulkan(true,created);
    if (!created.ok()) for (const auto &d : created.diagnostics) std::cerr << d.message << '\n';
    check(bool(device),"GPU-HEADLESS-DEVICE");
    auto r = sphere(device->id());
    std::shared_ptr<const ms::AssetProduct> retained;
    std::weak_ptr<cd::Device> weak = device;
    {
        gc::SolidBackend backend(device);
        auto result = backend.generate(job,r); valid(result,"GPU-HEADLESS-SPHERE");
        check(result.envelope() == job,"GPU-ENVELOPE");
        retained = result.value();
        const auto &mesh = retained->document.meshes.at(0);
        check(!mesh.indices.empty() && mesh.indices.size()%3 == 0,"GPU-TOPOLOGY");
        // Pinned tolerance: <=0.02 metres sphere radial error for 0.25m cells;
        // normal alignment >=0.99. Not adjusted to match observed output.
        for (size_t i = 0; i < mesh.positions.size(); ++i) {
            const auto p = mesh.positions[i]; const auto n = mesh.normals[i];
            float length = std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z);
            check(std::abs(length-1) <= .02f,"GPU-SPHERE-ERROR-0.02m");
            check((p.x*n.x+p.y*n.y+p.z*n.z)/length >= .99f,"GPU-SPHERE-NORMAL");
        }
        for (int i = 0; i < 3; ++i) {
            auto repeat = backend.generate(job,r); valid(repeat,"GPU-REPEAT");
            check(repeat.value()->document.meshes[0].indices == mesh.indices,"GPU-DETERMINISTIC-TOPOLOGY");
            const auto &positions = repeat.value()->document.meshes[0].positions;
            for (size_t k = 0; k < positions.size(); ++k) check(positions[k].x == mesh.positions[k].x &&
                positions[k].y == mesh.positions[k].y && positions[k].z == mesh.positions[k].z,"GPU-DETERMINISTIC-POSITIONS");
        }
        for (auto shape : {gc::Shape::box,gc::Shape::rounded_box,gc::Shape::ellipsoid,gc::Shape::capsule}) {
            auto primitive = r; primitive.elements[0].shape = shape;
            primitive.elements[0].dimensions = {.75f,.5f,.5f};
            if (shape == gc::Shape::rounded_box) primitive.elements[0].rounding = .1f;
            valid(backend.generate(job,primitive),"GPU-LEGACY-SHAPES");
        }
        auto subtraction = r;
        subtraction.elements[0].shape = gc::Shape::box;
        gc::SolidElement cut; cut.shape = gc::Shape::sphere; cut.combine = gc::Combine::subtract;
        cut.dimensions = {.65f,.65f,.65f}; subtraction.elements.push_back(cut);
        auto carved = backend.generate(job,subtraction); valid(carved,"GPU-ORDERED-SUBTRACTION");
        bool cavity = false;
        for (auto p : carved.value()->document.meshes[0].positions)
            if (std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z)<.7f) cavity = true;
        check(cavity,"GPU-SUBTRACTION-CAVITY");
        auto empty = r; auto same = empty.elements[0]; same.combine = gc::Combine::subtract; empty.elements.push_back(same);
        auto none = backend.generate(job,empty); valid(none,"GPU-EMPTY-SUCCESS");
        check(none.value()->document.meshes.empty(),"GPU-EMPTY-NO-PARTIAL");
        ms::EvaluationOptions tiny; tiny.budget.vertices = tiny.budget.indices = 3;
        fails(backend.generate(job,r,tiny),mc::ErrorCode::resource_limit,"GPU-CAPACITY-OVERFLOW");
        ms::CancellationSource cancel;
        ms::JobControl control{cancel.token(),[&](const ms::Progress &p) { if (p.stage == "complete") cancel.cancel(); }};
        fails(backend.generate(job,r,{},control),mc::ErrorCode::cancelled,"GPU-CANCEL-BEFORE-PUBLISH");
        std::shared_ptr<const ms::AssetProduct> destination = retained;
        auto late = job; ++late.generation_epoch;
        check(!ms::admit(result,late,destination).ok() && destination == retained,"GPU-LATE-ADMISSION");
        cd::Request mismatch; mismatch.device = device->id()+1;
        ms::JobMonitor monitor({},{});
        check(!device->execute(job,mismatch,monitor).ok(),"GPU-DEVICE-DIRECT-MISMATCH");
        for (const auto &message : cd::validation_messages(device)) std::cerr << message << '\n';
        check(cd::validation_errors(device) == 0,"GPU-SYNCHRONIZATION-VALIDATION");
    }
    device.reset();
    check(weak.expired() && retained && !retained->document.meshes[0].positions.empty(),"GPU-CPU-OWNERSHIP");
    std::cout << "PASS GPU headless generation, legacy shapes, order, overflow, sync validation, CPU ownership\n";
}
#endif
}
int main() {
    gc::SolidBackend unavailable;
    fails(unavailable.generate(job,sphere(0)),mc::ErrorCode::unsupported_capability,"GPU-UNAVAILABLE");
    check(unavailable.capabilities().empty(),"GPU-NO-FALSE-CAPABILITY");
    auto fake = std::make_shared<Fake>();
    auto request = sphere(fake->id());
#ifndef MATTER_GEOMETRY_COMPUTE
    gc::SolidBackend disabled(fake);
    fails(disabled.generate(job,request),mc::ErrorCode::unsupported_capability,"GPU-DISABLED");
    check(fake->calls == 0,"GPU-DISABLED-NO-DISPATCH");
    std::cout << "PASS GPU-DISABLED / GPU-UNAVAILABLE\n";
#else
    memory_only_validation();
    admission(); gpu();
#endif
}
