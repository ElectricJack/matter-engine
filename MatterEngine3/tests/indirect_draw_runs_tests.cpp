#include "render/indirect_draw_runs.h"
#include "check.h"
#include <vector>
#include <random>

struct Range {
    uint32_t first_command=0, command_count=0, part_slot=0;
    bool raster_water_surface=false;
};
int main() {
    const std::vector<Range> spans{{0,4,1,false},{4,4,2,false},{8,4,3,true},
                                  {12,4,4,false},{16,4,5,false}};
    std::vector<Range> runs;
    viewer::for_each_opaque_indirect_run(spans.data(), uint32_t(spans.size()),20,100,
        [&](Range r){runs.push_back(r);});
    CHECK(runs.size()==2 && runs[0].first_command==0 && runs[0].command_count==8 &&
          runs[1].first_command==12 && runs[1].command_count==8,
          "touching opaque parts merge while excluded water leaves a gap");
    std::mt19937 rng(42);
    for(unsigned trial=0;trial<1000;++trial) {
        std::vector<Range> input;
        std::vector<uint32_t> expected,actual;
        const uint32_t limit=1+rng()%17;
        for(unsigned n=0;n<50;++n) {
            Range r{rng()%110,rng()%12,n,(rng()%5)==0}; input.push_back(r);
            if(!r.raster_water_surface && r.first_command<=100 && r.command_count<=100-r.first_command)
                for(uint32_t i=0;i<r.command_count;++i)expected.push_back(r.first_command+i);
        }
        const auto calls=viewer::for_each_opaque_indirect_run(input.data(),uint32_t(input.size()),100,limit,
            [&](Range r){
                CHECK(r.command_count && r.command_count<=limit && !r.raster_water_surface,
                      "emitted draw respects device limit and opaque classification");
                for(uint32_t i=0;i<r.command_count;++i)actual.push_back(r.first_command+i);
            });
        CHECK(actual==expected,"batching preserves exact filtered draw order and multiplicity");
        CHECK(calls<=actual.size(),"each issued call contains work");
    }
    const Range edge[]{{UINT32_MAX-12,6,0,false},{UINT32_MAX-6,6,1,false}};
    runs.clear();
    viewer::for_each_opaque_indirect_run(edge,2,UINT32_MAX,7,[&](Range r){runs.push_back(r);});
    CHECK(runs.size()==2 && runs[0].command_count==7 && runs[1].command_count==5 &&
          runs[1].first_command==UINT32_MAX-5,"near-limit spans merge and split without overflow");
    CHECK(viewer::for_each_opaque_indirect_run<Range>(nullptr,0,0,0,[](Range){})==0,
          "empty input emits no draw");
    return check_summary();
}
