#include "geometry_properties.h"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
uint32_t argument(const char *text) {
    size_t used=0; auto value=std::stoull(text,&used,0);
    if(text[used] || text[0]=='-' || value>std::numeric_limits<uint32_t>::max()) throw std::runtime_error("invalid uint32 argument");
    return uint32_t(value);
}
}
int main(int argc,char **argv) {
    uint32_t seed=0x51a7c0deu; unsigned group=0;
    try {
#ifdef MATTER_GEOMETRY_STANDALONE_FUZZ
        if(argc>4) throw std::runtime_error("usage: matter_geometry_fuzz [iterations [seed [group]]]");
        uint32_t count=argc>1?argument(argv[1]):10000;
        if(argc>2) seed=argument(argv[2]);
        constexpr unsigned groups=MATTER_GEOMETRY_EXPECT_MODIFIERS ? 5 : 4;
        for(uint32_t i=0;i<count;++i) {
            group=argc>3 ? argument(argv[3]) : i%groups;
            geometry_properties::run(group,seed);
            seed=seed*1664525u+1013904223u;
        }
#else
        if(argc<2 || argc>3) throw std::runtime_error("usage: matter_geometry_property_tests <group 0..4> [seed]");
        group=argument(argv[1]);
        if(argc==3) {
            seed=argument(argv[2]); geometry_properties::run(group,seed);
            std::printf("geometry property replay passed\n"); return 0;
        }
        if(group==0) {
            // Cover every primitive, both cone tips, and the original failing
            // apexB seed explicitly; a short random prefix misses cylinders.
            const uint32_t seeds[]={1,0x51a7c0deu,0x0e7a32a5u,0x9aee43c0u,0xa746e41fu,0x40f3e2f2u,0x4b9f81c3u};
            for(auto value:seeds) { seed=value; geometry_properties::run(group,seed); }
            std::printf("geometry primitive properties passed\n"); return 0;
        }
        // Fixed schedule, stable across STL/compiler versions; no clock or disk.
        const unsigned count=group==1 ? 12 : group==2 ? 2 : 4;
        for(unsigned i=0;i<count;++i) {
            geometry_properties::run(group,seed);
            seed=seed*1664525u+1013904223u;
        }
#endif
        std::printf("geometry properties passed\n"); return 0;
    } catch(const std::exception &e) {
        std::fprintf(stderr,"geometry property failure: group=%u seed=0x%08x: %s\n",group,seed,e.what()); return 1;
    }
}
