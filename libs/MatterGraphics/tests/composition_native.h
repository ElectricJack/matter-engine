// Included by vulkan_tests.cpp to reuse its validation-enabled native device.
#include "../examples/image_capture.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
namespace {
constexpr double pi = 3.14159265358979323846;
using D3 = std::array<double, 3>;
double dot3(D3 a, D3 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
D3 unit(D3 a) { double n=std::sqrt(dot3(a,a)); for(auto &v:a)v/=n; return a; }
mg::FloatImage solid(uint32_t w, uint32_t h, mg::Rgba value) {
  return {w,h,1,std::vector<mg::Rgba>(size_t(w)*h,value)};
}
mg::CompositionInputs plane() {
  mg::CompositionInputs in;
  constexpr uint32_t w=65,h=33;
  in.albedo=solid(w,h,{0,0,0,1}); in.normal=solid(w,h,{0,0,1,0});
  in.orm=solid(w,h,{0.5f,0,1,0});
  const float near=in.camera.near_metres, far=in.camera.far_metres;
  in.depth=solid(w,h,{near*(far-2)/(2*(far-near)),0,0,0});
  in.identity={w,h,std::vector<std::array<uint32_t,2>>(size_t(w)*h,{0,7})};
  in.diffuse=solid(w,h,{0,0,0,0}); in.visibility=solid(w,h,{1,1,1,1});
  in.camera.aspect_ratio=float(w)/h;
  in.camera.tan_half_fov=0.5f;
  for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
    size_t i=size_t(y)*w+x;
    in.albedo.pixels[i]={0.15f+0.7f*float(x)/float(w-1),0.3f,0.15f+0.5f*float(y)/float(h-1),1};
  }
  return in;
}
D3 view_at(const mg::CompositionInputs &in, size_t i) {
  const double u=(i%in.albedo.width+0.5)/in.albedo.width;
  const double v=(i/in.albedo.width+0.5)/in.albedo.height;
  return unit({-(2*u-1)*in.camera.aspect_ratio*in.camera.tan_half_fov,
               -(1-2*v)*in.camera.tan_half_fov,1});
}
// Independent double-precision plane oracle: receiver z=-2, camera at origin.
// A closed-form BRDF evaluates actual albedo/normal/material/light inputs;
// expected lit colors are never supplied to the GPU as a final-color texture.
D3 sun_reference(const mg::CompositionInputs &in, size_t i) {
  const auto &a=in.albedo.pixels[i], &orm=in.orm.pixels[i];
  const double rough=orm[0], metal=orm[1];
  D3 view=view_at(in,i), half=unit({view[0],view[1],view[2]+1});
  const double nv=view[2], nh=half[2], vh=dot3(view,half);
  double alpha=std::max(rough*rough,0.0004), a2=alpha*alpha;
  const double den=nh*nh*(a2-1)+1, distribution=a2/std::max(pi*den*den,1e-8);
  const double gv=2*nv/(nv+std::sqrt(a2+(1-a2)*nv*nv));
  const auto &vis=in.visibility.pixels[in.visibility.pixels.size()==1?0:i];
  const auto &sun=in.environment.direct_sun;
  const double intensity[3]={sun.x,sun.y,sun.z}; D3 out{};
  for(int c=0;c<3;++c) {
    double f=a[c]+(1-a[c])*std::pow(1-vh,5);
    out[c]=(a[c]*(1-metal)+metal*f*distribution*gv/(4*nv))*
      intensity[c]*vis[c]*(1-0.35*rough);
  }
  return out;
}
D3 local_reference(const mg::CompositionInputs &in, size_t i) {
  D3 view=view_at(in,i), pos{-2*view[0]/view[2],-2*view[1]/view[2],-2}, result{};
  for(int c=0;c<3;++c)pos[c]+=c==0?in.camera.position.x:c==1?in.camera.position.y:in.camera.position.z;
  const auto &a=in.albedo.pixels[i], &orm=in.orm.pixels[i];
  double rough=orm[0],metal=orm[1],nv=view[2];
  for(const auto &light:in.local_lights) {
    D3 delta{light.position.x-pos[0],light.position.y-pos[1],light.position.z-pos[2]};
    double d2=dot3(delta,delta); if(d2<=1e-20)continue;
    D3 l=unit(delta); double nl=std::max(l[2],0.0); if(nl<=0)continue;
    double cutoff=std::max(1-d2/(double(light.range_metres)*light.range_metres),0.0);
    double attenuation=cutoff*cutoff/(d2+double(light.source_radius_metres)*light.source_radius_metres);
    if(light.kind==ms::LightKind::spot) {
      D3 dir=unit({light.direction.x,light.direction.y,light.direction.z});
      double cos_theta=-dot3(l,dir), width=light.cos_inner-light.cos_outer;
      double cone=width<=1e-7?(cos_theta>=light.cos_inner?1.0:0.0):
        std::clamp((cos_theta-light.cos_outer)/width,0.0,1.0);
      if(width>1e-7)cone=cone*cone*(3-2*cone);
      attenuation*=cone;
    }
    D3 half=unit({view[0]+l[0],view[1]+l[1],view[2]+l[2]});
    double nh=half[2],vh=dot3(view,half),alpha=std::max(rough*rough,0.0004),a2=alpha*alpha;
    double den=nh*nh*(a2-1)+1,D=a2/std::max(pi*den*den,1e-8);
    auto g=[a2](double n){return 2*n/(n+std::sqrt(a2+(1-a2)*n*n));};
    const double color[3]={light.color.x,light.color.y,light.color.z};
    for(int c=0;c<3;++c) {
      double f0=0.04*(1-metal)+a[c]*metal,f=f0+(1-f0)*std::pow(1-vh,5);
      double spec=f*D*g(nv)*g(nl)/std::max(4*nv*nl,1e-6);
      double diff=(1-f)*(1-metal)*a[c]/pi;
      result[c]+=(spec+diff)*color[c]*attenuation*nl;
    }
  }
  return result;
}
double display_reference(double value, const mg::CompositionInputs &in, size_t i) {
  if(in.view!=mg::CompositionView::shaded)return std::clamp(value,0.0,1.0);
  double x=std::max(value*std::exp2(std::clamp(double(in.exposure_ev),-6.0,6.0)),0.0);
  double aces=std::clamp(x*(2.51*x+0.03)/(x*(2.43*x+0.59)+0.14),0.0,1.0);
  double code=aces<0.0031308?12.92*aces:1.055*std::pow(aces,1/2.4)-0.055;
  constexpr int ranks[64]={37,12,54,1,46,27,61,8,18,43,5,58,31,50,14,40,63,22,35,10,48,3,56,29,16,45,7,60,25,52,11,38,33,0,47,20,57,15,42,30,9,53,24,62,4,36,19,51,41,13,55,28,59,6,44,21,26,49,2,39,17,34,23,32};
  int rank=ranks[((i/in.albedo.width)&7)*8+((i%in.albedo.width)&7)];
  return std::clamp(code+(rank-31.5)/31.5*(0.5/255),0.0,1.0);
}
void matched(mg::VulkanComposition &backend,const std::string &name,const mg::CompositionInputs &in,
             const std::function<D3(size_t)> &reference) {
  mg::CompositionOutput out; valid(backend.render(in,out),name.c_str());
  check(out.width==in.albedo.width&&out.height==in.albedo.height,"COMP output extent");
  std::vector<mg::Rgba> expected(out.display_srgb.size()); double max_hdr=0,max_display=0;
  for(size_t i=0;i<out.linear_hdr.size();++i) {
    D3 hdr=reference(i); expected[i][3]=1;
    for(int c=0;c<3;++c) {
      expected[i][c]=float(display_reference(hdr[c],in,i));
      double error=std::abs(out.linear_hdr[i][c]-hdr[c]);
      double display_error=std::abs(out.display_srgb[i][c]-expected[i][c]);
      max_hdr=std::max(max_hdr,error); max_display=std::max(max_display,display_error);
      if(error>3e-4+3e-4*std::abs(hdr[c])||display_error>3e-4) {
        std::cerr<<name<<" pixel="<<i<<" channel="<<c<<" HDR="<<out.linear_hdr[i][c]
                 <<" expected="<<hdr[c]<<" display="<<out.display_srgb[i][c]<<" expected="<<expected[i][c]<<'\n';
        check(false,"COMP matched pixel");
      }
    }
    check(out.linear_hdr[i][3]==1&&out.display_srgb[i][3]==1,"COMP alpha");
  }
  ppm(name+"-actual",out.width,out.height,out.display_srgb); ppm(name+"-reference",out.width,out.height,expected);
  std::cout<<"MATCH "<<name<<" pixels="<<out.linear_hdr.size()<<" hdr_max="<<max_hdr<<" display_max="<<max_display<<'\n';
}
void exercise_composition(const std::shared_ptr<NativeDevice> &native) {
  mg::VulkanComposition backend(native->binding(native));
  auto in=plane(); in.environment.direct_sun={2,1.5f,1}; in.exposure_ev=-1;
  for(size_t i=0;i<in.orm.pixels.size();++i) {
    in.orm.pixels[i][0]=0.2f+0.6f*float(i/in.albedo.width)/float(in.albedo.height-1);
    uint32_t x=uint32_t(i%in.albedo.width);
    in.visibility.pixels[i]=x<21?mg::Rgba{0,0,0,1}:x<43?mg::Rgba{0.3f,0.5f,0.7f,1}:mg::Rgba{1,1,1,1};
  }
  matched(backend,"dielectric-colored-shadow",in,[&](size_t i){return sun_reference(in,i);});
  for(auto &orm:in.orm.pixels){orm[0]=0.45f;orm[1]=1;}
  matched(backend,"metallic-ggx-shadow",in,[&](size_t i){return sun_reference(in,i);});
  in=plane(); in.environment.direct_sun={0,0,0};
  mg::LocalLight point; point.color={8,5,3};point.range_metres=10;point.source_radius_metres=0.1f;
  in.local_lights={point};
  matched(backend,"point-light-brdf",in,[&](size_t i){return local_reference(in,i);});
  in.local_lights[0].kind=ms::LightKind::spot;in.local_lights[0].cos_outer=0.8f;in.local_lights[0].cos_inner=0.95f;
  matched(backend,"spot-light-cone",in,[&](size_t i){return local_reference(in,i);});
  in.camera.position={3,4,5}; in.local_lights[0].position={3,4,5};
  matched(backend,"translated-camera-light",in,[&](size_t i){return local_reference(in,i);});
  in.camera.position={};
  in.local_lights.clear();in.lighting.diffuse_rt_multiplier=0.75f;
  in.diffuse=solid(in.albedo.width,in.albedo.height,{0.2f,0.4f,0.6f,0});
  in.specular.pixels[0]={0.15f,0.2f,0.3f,0};in.transmission.pixels[0]={0.8f,0.7f,0.4f,0.5f};
  in.local_direct.pixels[0]={0.3f,0.25f,0.1f,0}; in.local_direct_owner=mg::LocalDirectOwner::ray_traced_product;
  in.local_lights={point}; // Must not double-count this raster light.
  for(auto &n:in.normal.pixels)n[3]=2;
  in.materials[0].emission_strength={0.25f,0.4f,0.6f,0};
  auto products=[&](size_t i) {
    D3 out{};for(int c=0;c<3;++c)out[c]=in.diffuse.pixels[i][c]*0.75*0.5+
      in.materials[0].emission_strength[c]*3+in.specular.pixels[0][c]+
      in.transmission.pixels[0][c]*0.5+in.local_direct.pixels[0][c];return out;
  };
  matched(backend,"rt-products-emission-single-owner",in,products);
  in.volume_enabled=true;in.integrated_volume={2,2,2,std::vector<mg::Rgba>(8,{0.2f,0.15f,0.1f,0.25f})};
  matched(backend,"integrated-volume",in,[&](size_t i){D3 out=products(i);for(int c=0;c<3;++c)out[c]=out[c]*0.25+in.integrated_volume.pixels[0][c];return out;});
  in=plane();in.environment.direct_sun={0,0,0};in.environment.irradiance_sh.pixels[0]={0.2f,0.4f,0.8f,0};
  matched(backend,"environment-irradiance",in,[&](size_t i){D3 out{};for(int c=0;c<3;++c)out[c]=in.albedo.pixels[i][c]*0.282095*pi*in.environment.irradiance_sh.pixels[0][c];return out;});
  in.environment.irradiance_sh.pixels[0]={0,0,0,0}; in.lighting.diffuse_rt_multiplier=1;
  in.diffuse=solid(17,9,{0.2f,0.4f,0.6f,0});
  matched(backend,"reduced-diffuse-odd-extent",in,[&](size_t i){D3 out{};for(int c=0;c<3;++c)out[c]=in.albedo.pixels[i][c]*in.diffuse.pixels[0][c];return out;});
  in=plane();in.environment.direct_sun={0,0,0};in.environment.sky_view.pixels[0]={0.1f,0.3f,0.7f,1};
  for(auto &n:in.normal.pixels)n={0,0,0,0};
  matched(backend,"physical-sky-product",in,[&](size_t){return D3{0.1f,0.3f,0.7f};});
  in=plane();in.materials[0].scattering={0.2f,0.7f,0.1f,0.5f};in.materials[0].scattering_shape={0.25f,0,0,0};in.materials[0].flags_misc[0]=1;
  for(size_t i=0;i<in.normal.pixels.size();++i) {in.normal.pixels[i]={0,0,-1,0};if(i%in.albedo.width<32)in.identity.pixels[i][0]=0x80000000u;}
  matched(backend,"subsurface-impostor-mask",in,[&](size_t i){D3 out{};if(!(in.identity.pixels[i][0]&0x80000000u))for(int c=0;c<3;++c)out[c]=in.materials[0].scattering[c]*0.5*(1-0.35*0.5);return out;});
  in=plane(); in.environment.cloud_state={1,1,0,100};
  for(auto &m:in.environment.cloud_world_to_uvw) {
    m.m[0]=m.m[5]=m.m[10]=0.05f; m.m[3]=m.m[7]=m.m[11]=0.5f;
  }
  // Distinct ping values prove selector binding and row-major translation.
  in.environment.cloud_optical_depth[0].pixels[0][0]=0;
  in.environment.cloud_optical_depth[1].pixels[0][0]=float(std::log(2.0));
  matched(backend,"cloud-optical-depth",in,[&](size_t i){D3 out=sun_reference(in,i);for(auto &v:out)v*=0.5;return out;});
  in=plane();in.environment.direct_sun={0,0,0};in.environment.sky_view.pixels[0]={0.1f,0.3f,0.7f,1};
  in.materials[0].transmission[0]=0.8f;
  matched(backend,"native-glass-fallback",in,[&](size_t){return D3{0.1f*double(0.8f),0.3f*double(0.8f),0.7f*double(0.8f)};});
  in=plane();in.view=mg::CompositionView::albedo;in.exposure_ev=-6;
  matched(backend,"debug-display-passthrough",in,[&](size_t i){const auto &a=in.albedo.pixels[i];return D3{a[0],a[1],a[2]};});
  mg::CompositionOutput unchanged;unchanged.width=123;
  in.required_capabilities={{"matter.graphics.ray-tracing-producer",1}};
  check(!backend.render(in,unchanged).ok()&&unchanged.width==123,"COMP reject unsupported producer atomically");
  mg::CompositionLimits tiny; tiny.max_gpu_bytes=16;
  mg::VulkanComposition bounded(native->binding(native),tiny);
  in.required_capabilities.clear();
  auto limited=bounded.render(in,unchanged);
  check(!limited.ok()&&limited.diagnostics[0].code==mc::ErrorCode::resource_limit&&unchanged.width==123,
        "COMP driver allocation budget leaves output unchanged");
  std::cout<<"PASS COMP-NATIVE shader source parity; selected appearance fixtures; unsupported capability\n";
}
} // namespace
