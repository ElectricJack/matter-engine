#include <matter/asset/surface.h>
#include <matter/asset/archive.h>
#if MATTER_ASSET_SURFACE_NATIVE
#include <matter/asset/surface_tape.h>
#endif
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace matter;
using namespace matter::asset;
namespace {
const services::JobEnvelope job{"surface-fixture", "revision-1", 7};
void check(bool ok, const char *message) {
  if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template<class R> void success(const R &r) {
  if (!r.ok()) {
    for (const auto &d : r.diagnostics()) std::cerr << d.message << '\n';
    check(false, "expected success");
  }
}
template<class R> void error(const R &r, data::ErrorCode code) {
  check(!r.ok() && !r.value(), "failure publishes no product");
  check(!r.diagnostics().empty() && r.diagnostics()[0].code == code, "diagnostic code");
}
SurfaceSettings settings() {
  SurfaceSettings s;
  s.asset_id = {"fixture.surface"}; s.material_id = {"fixture.material"};
  s.provenance.id = {"fixture.source"}; s.provenance.uri = "fixture:surface";
  s.width = 2; s.height = 1;
  s.wrap_u = data::WrapMode::mirror; s.mag_filter = data::FilterMode::nearest;
  return s;
}
SurfaceImage image() {
  SurfaceImage r{settings(), std::vector<SurfaceTexel>(2)};
  r.texels[0].base_color = {0.0031308f, .18f, 1};
  r.texels[0].orm = {1,.25f,.75f}; r.texels[0].emissive = {.5f,0,.003f};
  r.texels[1].base_color = {1,0,0}; r.texels[1].normal = {0,1,0};
  return r;
}
services::EvaluationOptions native_options() {
  services::EvaluationOptions o;
  o.validation.supported_capabilities.push_back({native_surface_capability,1});
  return o;
}
const Bytes &bytes(const services::AssetProduct &p, const data::SemanticId &id) {
  for (const auto &r : p.resources.resources) if (r.description.id == id) return *r.bytes;
  check(false,"missing resource"); std::abort();
}
float scalar(const Bytes &b, size_t at) {
  uint32_t n = uint32_t(b.at(at)) | uint32_t(b.at(at+1))<<8 | uint32_t(b.at(at+2))<<16 | uint32_t(b.at(at+3))<<24;
  float f; std::memcpy(&f,&n,4); return f;
}
void near(float a, float b) { check(std::abs(a-b) <= 1e-6f, "float parity within 1e-6"); }
void conventional() {
  auto input = image();
  auto r = bake_surface(job,input); success(r);
  check(r.envelope() == job && !r.value()->native, "envelope and conventional product");
  const auto &p = r.value()->asset;
  check(services::validate(p).ok() && p.document.textures.size()==4, "portable consumer validates");
  const auto &t = p.document.textures;
  check(t[0].color_space == data::ColorSpace::srgb && t[3].color_space == data::ColorSpace::srgb &&
        t[1].color_space == data::ColorSpace::linear && t[2].color_space == data::ColorSpace::linear,
        "explicit color spaces");
  check(bytes(p,t[0].resource) == Bytes({10,118,255,255,255,0,0,255}), "linear to sRGB and row order");
  check(bytes(p,t[1].resource) == Bytes({255,64,191,255,255,255,0,255}), "roughness G metallic B");
  check(bytes(p,t[2].resource) == Bytes({128,128,255,255,128,255,128,255}), "normal positive Y packing");
  check(bytes(p,t[3].resource) == Bytes({188,0,10,255,0,0,0,255}), "emissive sRGB");
  check(t[0].wrap_u == data::WrapMode::mirror && t[0].wrap_v == data::WrapMode::clamp &&
        t[0].mag_filter == data::FilterMode::nearest, "sampler preserved");
  const auto &m = p.document.materials[0];
  check(m.base_color.texture == t[0].id && m.normal_texture == t[2].id &&
        m.metallic_roughness_texture == t[1].id && m.emissive_texture == t[3].id &&
        m.metallic == 1 && m.roughness == 1 && m.emissive.x == 1, "texture bindings and neutral factors");
  input.texels.clear();
  check(bytes(p,t[0].resource).size()==8, "product owns pixels after input disposal");
  auto encoded = encode_prepared(job,p); success(encoded);
  auto restored = decode_prepared(job,*encoded.value()); success(restored);
  check(bytes(*restored.value(),t[0].resource) == bytes(p,t[0].resource), "portable archive preserves textures");
  std::cout << "AP-SURFACE-PBR AP-SURFACE-COLORSPACE AP-SURFACE-OWNER\n";
}
void native_and_rejections() {
  auto input = image();
  for (int feature = 0; feature < 5; ++feature) {
    auto unsupported = input;
    if (feature == 0) unsupported.texels[0].height_metres = .01f;
    if (feature == 1) unsupported.texels[0].orm.x = .5f;
    if (feature == 2) unsupported.texels[0].coverage = 0;
    if (feature == 3) unsupported.texels[0].base_color.x = 1.5f;
    if (feature == 4) unsupported.texels[0].emissive.x = 2;
    error(bake_surface(job,unsupported),data::ErrorCode::unsupported_capability);
  }
  input.settings.output = SurfaceOutput::native_float32;
  input.texels[0].height_metres = -.025f; input.texels[0].orm.x = .3f;
  input.texels[0].base_color.x = 1.5f; input.texels[1].coverage = 0;
  auto o = native_options();
  error(bake_surface(job,input),data::ErrorCode::unsupported_capability);
  auto r = bake_surface(job,input,o); success(r);
  const auto &n = *r.value()->native;
  const auto &p = r.value()->asset;
  check(n.width==2 && n.height==1 && n.material==input.settings.material_id, "typed native descriptor");
  near(scalar(bytes(p,n.base_color),0),1.5f);
  near(scalar(bytes(p,n.orm),0),.3f);
  near(scalar(bytes(p,n.height_metres),0),-.025f);
  check(bytes(p,n.coverage)==Bytes({1,0}), "coverage retained outside opaque baseline");
  check(!services::validate(p).ok() && services::validate(p,o.validation).ok(), "consumer must opt into native semantics");
  check(p.document.textures.empty() && p.document.extensions.size()==1 && p.document.extensions[0].required,
        "native resource semantics never advertised as baseline rgba8");
  auto encoded = encode_prepared(job,p,o); success(encoded);
  error(decode_prepared(job,*encoded.value()),data::ErrorCode::unsupported_capability);
  auto decoded = decode_prepared(job,*encoded.value(),o); success(decoded);
  std::string a,b;
  check(data::encode(p.document,a,o.validation).ok() && data::encode(decoded.value()->document,b,o.validation).ok() && a==b,
        "native semantic extension exact archive roundtrip");
  for (const auto &resource : p.resources.resources)
    check(bytes(*decoded.value(),resource.description.id)==*resource.bytes, "native resource exact archive roundtrip");
  input.settings.required_capabilities.push_back({"unknown.surface",1});
  error(bake_surface(job,input,o),data::ErrorCode::unsupported_capability);
  std::cout << "AP-SURFACE-NATIVE AP-SURFACE-NO-LOSS AP-SURFACE-CAPS AP-SURFACE-ARCHIVE\n";
}
void failures() {
  auto r = image();
  services::EvaluationOptions o;
  o.budget.output_bytes=31;
  error(bake_surface(job,r,o),data::ErrorCode::resource_limit);
  o={}; o.budget.working_bytes=1;
  error(bake_surface(job,r,o),data::ErrorCode::resource_limit);
  o={}; o.budget.operations=1;
  error(bake_surface(job,r,o),data::ErrorCode::resource_limit);
  o={}; o.algorithms.push_back({surface_bake_capability,2});
  error(bake_surface(job,r,o),data::ErrorCode::unsupported_capability);
  r.settings.width=UINT32_MAX; r.settings.height=UINT32_MAX;
  error(bake_surface(job,r),data::ErrorCode::invalid_input);
  r=image(); r.texels[0].normal={0,0,0};
  error(bake_surface(job,r),data::ErrorCode::invalid_input);
  r=image(); r.texels[0].height_metres=std::numeric_limits<float>::quiet_NaN();
  error(bake_surface(job,r),data::ErrorCode::invalid_input);
  r=image(); r.texels[0].coverage=2;
  error(bake_surface(job,r),data::ErrorCode::invalid_input);
  r=image(); r.settings.wrap_u=static_cast<data::WrapMode>(99);
  error(bake_surface(job,r),data::ErrorCode::invalid_input);
  r=image(); r.settings.output=static_cast<SurfaceOutput>(99);
  error(bake_surface(job,r),data::ErrorCode::unsupported_capability);
  services::CancellationSource cancel;
  services::JobControl c; c.cancellation=cancel.token(); cancel.cancel();
  error(bake_surface(job,image(),{},c),data::ErrorCode::cancelled);
  services::CancellationSource late;
  c.cancellation=late.token(); c.progress=[&](const auto &) {late.cancel();};
  error(bake_surface(job,image(),{},c),data::ErrorCode::cancelled);
  c={}; c.progress=[](const auto &) {throw std::runtime_error("callback failure");};
  error(bake_surface(job,image(),{},c),data::ErrorCode::backend_failure);
  auto good=bake_surface(job,image()); success(good);
  auto owner=good.value(); auto newer=job; ++newer.generation_epoch;
  check(!services::admit(good,newer,owner).ok() && owner==good.value(), "stale result preserves prior owner");
  error(bake_surface({},image()),data::ErrorCode::invalid_input);
  std::cout << "AP-SURFACE-INVALID AP-SURFACE-BUDGET AP-SURFACE-CANCEL AP-SURFACE-STALE\n";
}
#if MATTER_ASSET_SURFACE_NATIVE
const std::string flat_tape =
  "const 0.2\nconst 0.4\nconst 0.6\nconst 0.8\nconst 0.25\nconst 1\nconst 0\n"
  "material 8 r5\nsource 1 r0 r1 r2 r3 r4 r5 r6 0 0\n";
SurfaceTapeRequest tape_request() {
  return {settings(),flat_tape,std::vector<SurfaceBakePoint>(2)};
}
void tape() {
  auto r=tape_request(); auto portable=bake_surface_tape(job,r); success(portable);
  SurfaceImage expected{settings(),std::vector<SurfaceTexel>(2)};
  for(auto &t:expected.texels) {t.base_color={.2f,.4f,.6f}; t.orm={1,.8f,.25f};}
  auto reference=bake_surface(job,expected); success(reference);
  for(size_t i=0;i<4;++i)
    check(*portable.value()->asset.resources.resources[i].bytes==*reference.value()->asset.resources.resources[i].bytes,
          "tape evaluates conventional channels");
  r.settings.output=SurfaceOutput::native_float32;
  r.tape += "tint r5 r0 r1\nroughbias r4\nwetness r4\nmetallic r5\ncoat r5 r6 r6 r4 r1\n";
  auto native=bake_surface_tape(job,r,native_options()); success(native);
  const auto &n=*native.value()->native;
  const auto &p=native.value()->asset;
  near(scalar(bytes(p,n.base_color),0),.2f*.8875f*.75f+.25f);
  near(scalar(bytes(p,n.base_color),4),.4f*.2f*.8875f*.75f);
  near(scalar(bytes(p,n.orm),4),std::sqrt(.77f*.77f*.75f+.4f*.4f*.25f));
  near(scalar(bytes(p,n.orm),8),.75f);
  check(n.source_version==1 && n.source_tape.has_value(), "typed source version and resource");
  check(bytes(p,*n.source_tape)==Bytes(r.tape.begin(),r.tape.end()), "exact source program preservation");

  // Native face fixture: h=.25*x, dz=0. Tangent handedness affects only Y.
  r=tape_request(); r.settings.output=SurfaceOutput::native_float32;
  r.tape="input lx\nconst 0.5\nadd r0 r1\ninput lz\nadd r3 r1\nfootprint\nconst 0.8\nconst 0\nconst 1\nconst 0.25\nmul r0 r9\nmaterial 8 r8\nsource 1 r2 r4 r5 r6 r7 r8 r10 -0.05 0.05\nroughbias r5\n";
  r.points[0].position={.1f,0,.04f}; r.points[0].footprint_metres=.01f;
  r.points[1]=r.points[0]; r.points[1].tangent={0,1,0,-1};
  auto relief=bake_surface_tape(job,r,native_options()); success(relief);
  const auto &rn=*relief.value()->native; const auto &rp=relief.value()->asset;
  near(scalar(bytes(rp,rn.base_color),0),.6f);
  near(scalar(bytes(rp,rn.base_color),4),.54f);
  near(scalar(bytes(rp,rn.base_color),8),.01f);
  near(scalar(bytes(rp,rn.orm),4),.81f);
  near(scalar(bytes(rp,rn.height_metres),0),.025f);
  near(scalar(bytes(rp,rn.normal),0),-.25f/std::sqrt(1.0625f));
  near(scalar(bytes(rp,rn.normal),8),1/std::sqrt(1.0625f));
  near(scalar(bytes(rp,rn.normal),12),0);
  near(scalar(bytes(rp,rn.normal),16),-.25f/std::sqrt(1.0625f));
  near(rn.height_min_metres,-.05f); near(rn.height_max_metres,.05f);
  auto changed_job=job; changed_job.generation_epoch=99;
  auto repeat=bake_surface_tape(changed_job,r,native_options()); success(repeat);
  check(bytes(repeat.value()->asset,rn.normal)==bytes(rp,rn.normal), "generation does not change product identity");
  r.settings.output=SurfaceOutput::pbr_rgba8;
  error(bake_surface_tape(job,r),data::ErrorCode::unsupported_capability);
  r=tape_request(); r.tape="noise3 19 2 3 0.5 2 23 1 0.1\nconst 1\nconst 0\nmaterial 8 r1\nsource 1 r0 r0 r0 r1 r2 r1 r2 0 0\n";
  auto noise=bake_surface_tape(job,r); success(noise);
  auto noise_again=bake_surface_tape(job,r); success(noise_again);
  check(*noise.value()->asset.resources.resources[0].bytes==*noise_again.value()->asset.resources.resources[0].bytes,
        "existing seeded noise producer deterministic");
  std::cout << "AP-SURFACE-TAPE AP-SURFACE-APPEARANCE AP-SURFACE-RELIEF AP-SURFACE-NOISE\n";
}
void tape_failures() {
  auto r=tape_request();
  for (const auto &bad : {"const 0.2junk\n", "const nan\n", "const 1 trailing\n", "noise3 1 1 100000 0.5 2\n", "noise2 4294967296 1 2 0.5 2\n"}) {
    r.tape=std::string(bad)+flat_tape;
    error(bake_surface_tape(job,r),data::ErrorCode::invalid_input);
  }
  r.tape="unknown 1\n"+flat_tape;
  error(bake_surface_tape(job,r),data::ErrorCode::unsupported_capability);
  r.tape=flat_tape; r.tape.replace(r.tape.find("source 1"),8,"source 2");
  error(bake_surface_tape(job,r),data::ErrorCode::unsupported_version);
  for (const auto &input : {"wx", "receiver_material"}) {
    r.tape=std::string("input ")+input+"\nconst 1\nconst 0\nmaterial 8 r1\nsource 1 r0 r0 r0 r1 r2 r1 r2 0 0\n";
    error(bake_surface_tape(job,r),data::ErrorCode::unsupported_capability);
  }
  r.tape="const 1\nmaterial 8 r0\n";
  error(bake_surface_tape(job,r),data::ErrorCode::unsupported_capability);
  r.tape="noise2 1 1e30 2 0.5 2\nconst 1\nconst 0\nmaterial 8 r1\nsource 1 r0 r0 r0 r1 r2 r1 r2 0 0\n";
  error(bake_surface_tape(job,r),data::ErrorCode::unsupported_capability);
  r=tape_request(); r.points[0].tangent.w=0;
  error(bake_surface_tape(job,r),data::ErrorCode::invalid_input);
  r=tape_request(); r.points[0].normal.x=std::numeric_limits<float>::infinity();
  error(bake_surface_tape(job,r),data::ErrorCode::invalid_input);
  r=tape_request(); r.points[0].coverage=0;
  error(bake_surface_tape(job,r),data::ErrorCode::unsupported_capability);
  r.settings.output=SurfaceOutput::native_float32;
  auto hole=bake_surface_tape(job,r,native_options()); success(hole);
  check(bytes(hole.value()->asset,hole.value()->native->coverage)==Bytes({0,1}), "native hole coverage");
  r=tape_request(); services::EvaluationOptions tiny;
  tiny.budget.operations=r.tape.size()+1;
  error(bake_surface_tape(job,r,tiny),data::ErrorCode::resource_limit);
  tiny={}; tiny.budget.working_bytes=2048;
  error(bake_surface_tape(job,r,tiny),data::ErrorCode::resource_limit);
  services::CancellationSource cancel; services::JobControl c;
  c.cancellation=cancel.token(); c.progress=[&](const auto &){cancel.cancel();};
  error(bake_surface_tape(job,r,{},c),data::ErrorCode::cancelled);
  std::cout << "AP-SURFACE-TAPE-INVALID AP-SURFACE-TAPE-UNSUPPORTED AP-SURFACE-TAPE-BUDGET\n";
}
#endif
} // namespace
int main() {
  conventional(); native_and_rejections(); failures();
#if MATTER_ASSET_SURFACE_NATIVE
  tape(); tape_failures();
#endif
  return 0;
}
