#include "mesh_worker_protocol.h"
#include "mesh_retopo.hpp"
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc,char **argv) {
    namespace wire=matter::geometry::wire;
    try {
        if(argc!=4) return 2;
        if(std::string(argv[1])=="--probe") {
            std::ofstream out(argv[3],std::ios::binary); wire::put(out,wire::magic);
            out.close(); return out ? 0 : 2;
        }
        if(std::string(argv[1])!="--run") return 2;
        const auto bytes=std::filesystem::file_size(argv[2]);
        wire::require(bytes<=wire::hard_bytes+32 && bytes>=32);
        std::ifstream in(argv[2],std::ios::binary);
        wire::Limits limits{};
        const auto requested=wire::read_options(in,limits);
        auto mesh=wire::read(in,{UINT32_MAX,UINT32_MAX,bytes-32});
        wire::require(in.peek()==EOF);
        RetopoOptions options;
        options.target_ratio=requested.target_ratio; options.iterations=int(requested.iterations);
        options.seed=requested.seed; options.timeout_seconds=int(requested.timeout_seconds); options.threads=1;
        auto result=retopo(mesh,options);
        if(!result.ok) return 3;
        if(result.mesh.positions.size()>limits.vertices || result.mesh.indices.size()>limits.indices ||
            wire::size(result.mesh.positions.size(),result.mesh.indices.size())>limits.bytes)
            return wire::resource_exit;
        std::ofstream out(argv[3],std::ios::binary);
        wire::write(out,result.mesh,limits); out.close();
        return out ? 0 : 4;
    } catch(const std::bad_alloc &) { return wire::resource_exit; }
    catch(...) { return 5; }
}
