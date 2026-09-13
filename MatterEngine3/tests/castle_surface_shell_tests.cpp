// Native QuickJS -> direct triangle buffer -> baked CPU geometry contract.
#include "script_host.h"
#include "render/part_store.h"
#include "blas_manager.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("usage: castle_surface_shell_tests <repo> <fresh-output-dir>");
        namespace fs=std::filesystem;
        fs::path repo=argv[1],out=argv[2];
        if(fs::exists(out))throw std::runtime_error("output directory already exists");
        fs::create_directories(out/"parts");
        script_host::ScriptHost host;
        host.set_shared_lib_roots({(repo/"projects/world_demo/shared-lib").string()});
        script_host::BakeOptions options;options.parts_dir=out.string();options.retain_geometry=true;
        for(const std::string module:{"CastleBeamSurface","CastleFloorSurface","CastleWallSurface"}) {
            std::ifstream file(repo/"projects/world_demo/objects"/(module+".js"));
            if(!file)throw std::runtime_error("source missing");
            const std::string source((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
            const int shapes=module=="CastleBeamSurface"?12:module=="CastleFloorSurface"?3:10;
            for(int shape=0;shape<shapes;++shape) {
                const auto start=std::chrono::steady_clock::now();
                const auto result=host.bake_source(source,"{\"shape\":"+std::to_string(shape)+"}",options);
                const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                if(!result.error.ok)throw std::runtime_error(result.error.message);
                if(!host.last_buffer().ops.empty())throw std::runtime_error("shell emitted voxel operations");
                if(!result.geometry||!result.geometry->blas)throw std::runtime_error("missing retained geometry");
                const int wall_counts[]={12,12,12,44,64,20,20,20,76,256};
                const int expected=module=="CastleWallSurface"?wall_counts[shape]:module=="CastleBeamSurface"&&shape==5?172:44;
                const bool curved=module=="CastleWallSurface"&&shape>=8;
                bool has_uv=false,has_varying_normals=false;
                if(result.geometry->blas->get_total_triangle_count()!=expected)throw std::runtime_error("unexpected triangle count");
                for(const auto& entry:result.geometry->blas->get_entries()) {
                    if(entry->tri_extra.size()!=entry->triangles.size())throw std::runtime_error("missing triangle attributes");
                    for(size_t i=0;i<entry->triangles.size();++i) {
                        const auto& t=entry->triangles[i];const auto& e=entry->tri_extra[i];
                        has_uv |= std::fabs(e.uv0.x)+std::fabs(e.uv0.y)+std::fabs(e.uv1.x)+std::fabs(e.uv1.y)>1e-6f;
                        has_varying_normals |= std::fabs(e.N0.x-e.N1.x)+std::fabs(e.N0.z-e.N1.z)+std::fabs(e.N0.x-e.N2.x)+std::fabs(e.N0.z-e.N2.z)>1e-5f;
                        const float ax=t.vertex1.x-t.vertex0.x,ay=t.vertex1.y-t.vertex0.y,az=t.vertex1.z-t.vertex0.z;
                        const float bx=t.vertex2.x-t.vertex0.x,by=t.vertex2.y-t.vertex0.y,bz=t.vertex2.z-t.vertex0.z;
                        float nx=ay*bz-az*by,ny=az*bx-ax*bz,nz=ax*by-ay*bx;
                        const float length=std::sqrt(nx*nx+ny*ny+nz*nz);
                        if(!(length>1e-9f))throw std::runtime_error("degenerate emitted triangle");
                        nx/=length;ny/=length;nz/=length;
                        for(const auto n:{e.N0,e.N1,e.N2})
                            if(std::fabs(n.x*n.x+n.y*n.y+n.z*n.z-1)>1e-4f||n.x*nx+n.y*ny+n.z*nz<(curved?.999f:.9999f))
                                throw std::runtime_error("CPU emitter did not retain planar unit normals");
                    }
                }
                if(!has_uv)throw std::runtime_error("attributed metric UVs were lost");
                if(curved&&!has_varying_normals)throw std::runtime_error("radial curve corner normals were flattened");
                std::printf("surface_shell module=%s shape=%d triangles=%d bake_ms=%.3f bytes=%llu\n",module.c_str(),shape,expected,elapsed,(unsigned long long)fs::file_size(result.written_path));
            }
        }
        std::puts("castle_surface_shell_tests: PASS");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"castle_surface_shell_tests: FAIL: %s\n",error.what());return 1;}
}
