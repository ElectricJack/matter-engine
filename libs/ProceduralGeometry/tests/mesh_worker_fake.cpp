#include "mesh_worker_protocol.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <signal.h>

int main(int argc,char **argv) {
    namespace w=matter::geometry::wire;
    if(argc!=4) return 2;
    const std::string mode=std::filesystem::path(argv[0]).filename();
    std::ofstream out(argv[3],std::ios::binary);
    if(std::string(argv[1])=="--probe") {
        if(mode=="probe_hang") { sleep(10); return 1; }
        w::put(out,mode=="wrong" ? 0 : w::magic); return 0;
    }
    // Let the host tests prove a child actually ran and was reaped, rather than
    // accidentally passing only a pre-dispatch cancellation checkpoint.
    { std::ofstream pid(std::string(argv[0])+".pid"); pid<<getpid(); }
    if(mode=="crash") std::abort();
    if(mode=="hang") { sleep(30); return 1; }
    if(mode=="huge") { w::put(out,w::magic); w::put(out,UINT32_MAX); w::put(out,UINT32_MAX); return 0; }
    if(mode=="truncated") { w::put(out,w::magic); return 0; }
    if(mode=="flood") { for(;;) out.put('x'); }
    try {
        std::ifstream in(argv[2],std::ios::binary); w::Limits limits{};
        auto options=w::read_options(in,limits);
        if(mode=="options" && (options.target_ratio!=0.5f || options.iterations!=7 ||
            options.seed!=UINT32_MAX || options.timeout_seconds!=12)) return 6;
        auto mesh=w::read(in,{UINT32_MAX,UINT32_MAX,w::hard_bytes});
        if(mode=="material") mesh.triex[0].materialId=12345;
        w::write(out,mesh,limits);
        if(mode=="nan") { out.seekp(12); w::put(out,0x7fc00000); }
        if(mode=="index") { out.seekp(12+mesh.positions.size()*12); w::put(out,UINT32_MAX); }
        if(mode=="trailing") out.put('x');
        out.close();
        return mode=="nonzero" ? 3 : 0;
    } catch(...) { return 4; }
}
