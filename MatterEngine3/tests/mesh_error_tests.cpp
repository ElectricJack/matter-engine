#include "mesh_error.h"
#include "check.h"
#include <cmath>
#include <limits>

static std::vector<mesh_error::Triangle> square(float z=0) {
    return {{{mm::Vec3{0,0,z},{1,0,z},{1,1,z}}},{{mm::Vec3{0,0,z},{1,1,z},{0,1,z}}}};
}
int main() {
    using namespace mesh_error;
    Config config;config.tolerance=.001;Bounds result;std::string error;
    const auto original=square();
    uint64_t projection_tests=0;std::vector<uint32_t> nearest;
    CHECK(nearest_triangles(original,{{.75,.25,.3},{.25,.75,-.4},{.5,.5,0}},
          projection_tests,64,nearest,error),error.c_str());
    CHECK(nearest==std::vector<uint32_t>({0,1,0}),"indexed projection preserves regions and deterministic edge ties");
    const auto previous=nearest;
    CHECK(!nearest_triangles(original,{{.5,.5,1}},projection_tests,projection_tests,nearest,error) && nearest==previous,
          "projection budget failure preserves prior output");
    std::vector<Triangle> separated;
    for(int i=0;i<128;++i)for(auto t:square()) {
        for(auto& p:t)p.x+=i*4.f;separated.push_back(t);
    }
    projection_tests=0;
    CHECK(nearest_triangles(separated,{{256.75,.25,1}},projection_tests,32,nearest,error) && nearest[0]==128,
          "spatial projection finds the correct triangle within a sublinear test budget");
    CHECK(measure(original,original,config,result,error),error.c_str());
    CHECK(result.lower==0 && result.upper<1e-9,"identical surfaces have zero geometric error");
    CHECK(measure(original,square(.125f),config,result,error),error.c_str());
    CHECK(std::abs(result.lower-.125)<1e-10 && result.upper>=.125 && result.upper<.126,
          "parallel surfaces enclose the analytical distance");
    std::vector<Triangle> rim;
    const mm::Vec3 corners[]={{0,0,0},{1,0,0},{1,1,0},{0,1,0}};
    for(int i=0;i<4;++i) rim.push_back({corners[i],corners[(i+1)%4],corners[i]});
    CHECK(measure(original,rim,config,result,error),error.c_str());
    CHECK(result.lower>=.499 && result.upper>=.5 && result.upper<=.502,
          "interior holes are detected when every original corner has zero error");
    const double first_lower=result.lower,first_upper=result.upper;
    CHECK(measure(rim,original,config,result,error),error.c_str());
    CHECK(result.lower>=.499 && result.upper>=.5 && result.upper<=.502 &&
          std::abs(result.lower-first_lower)<.002 && std::abs(result.upper-first_upper)<.002,
          "error bounds cover both directions");
    auto moved=original,moved_rim=rim;
    for(auto* mesh:{&moved,&moved_rim}) for(auto& t:*mesh) for(auto& p:t) p={100-2*p.y,20+2*p.x,3+2*p.z};
    config.tolerance=.002;
    CHECK(measure(moved,moved_rim,config,result,error),error.c_str());
    CHECK(result.lower>=.998 && result.upper>=1 && result.upper<=1.004,
          "surface error follows translation rotation and uniform scale");
    config.max_depth=0;
    CHECK(measure(original,rim,config,result,error),error.c_str());
    CHECK(result.depth_limited && result.upper>=.5,"subdivision limit retains a conservative upper bound");
    config={};config.max_queries=1;result.lower=123;
    CHECK(!measure(original,rim,config,result,error) && result.lower==123,"query budget failure leaves the prior result intact");
    config={};auto invalid=original;invalid[0][0].x=std::numeric_limits<float>::quiet_NaN();
    CHECK(!measure(invalid,original,config,result,error),"invalid geometry is rejected before BVH construction");
    const std::vector<Triangle> point_a{{mm::Vec3{2,3,4},{2,3,4},{2,3,4}}},point_b{{mm::Vec3{5,7,4},{5,7,4},{5,7,4}}};
    CHECK(measure(point_a,point_b,config,result,error) && std::abs(result.lower-5)<1e-9 && result.upper>=5,
          "degenerate triangles retain point and segment distance semantics");
    return check_summary();
}
