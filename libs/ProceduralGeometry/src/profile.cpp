#include "internal.h"

namespace matter::geometry::detail {
namespace {
double orient(poly_tri::P2 a, poly_tri::P2 b, poly_tri::P2 c) {
    return (double(b.x)-a.x)*(double(c.y)-a.y)-(double(b.y)-a.y)*(double(c.x)-a.x);
}
bool on(poly_tri::P2 a, poly_tri::P2 b, poly_tri::P2 p) {
    return orient(a,b,p) == 0 && p.x >= std::min(a.x,b.x) && p.x <= std::max(a.x,b.x) &&
        p.y >= std::min(a.y,b.y) && p.y <= std::max(a.y,b.y);
}
bool intersects(poly_tri::P2 a, poly_tri::P2 b, poly_tri::P2 c, poly_tri::P2 d) {
    double x=orient(a,b,c), y=orient(a,b,d), z=orient(c,d,a), w=orient(c,d,b);
    return (((x>0 && y<0) || (x<0 && y>0)) && ((z>0 && w<0) || (z<0 && w>0))) ||
        on(a,b,c) || on(a,b,d) || on(c,d,a) || on(c,d,b);
}
double area(const poly_tri::Contour &c) {
    double a=0;
    for (size_t i=0;i<c.size();++i) {
        const auto &p=c[i], &q=c[(i+1)%c.size()];
        a+=double(p.x)*q.y-double(q.x)*p.y;
    }
    return a/2;
}
bool inside(const poly_tri::Contour &c, poly_tri::P2 p) {
    bool result=false;
    for(size_t i=0,j=c.size()-1;i<c.size();j=i++) {
        const auto &a=c[i], &b=c[j];
        if(on(a,b,p)) return false;
        if((a.y>p.y)!=(b.y>p.y) &&
            double(p.x)<(double(b.x)-a.x)*(double(p.y)-a.y)/(double(b.y)-a.y)+a.x) result=!result;
    }
    return result;
}
}

CheckedProfile profile(const data::RecipeNode &node, jobs::JobMonitor &monitor, jobs::ResourceUsage &usage) {
    const auto &outer=array(member(node.parameters,"points"));
    const auto &parameters=std::get<data::Value::Object>(node.parameters.data);
    const data::Value::Array *holes=nullptr;
    if(parameters.count("holes")) holes=&array(parameters.at("holes"));
    uint64_t count=outer.size();
    if(holes) for(const auto &h:*holes) {
        if(array(h).size()>65536-count) reject(data::ErrorCode::resource_limit,"profile point limit exceeded");
        count+=array(h).size();
    }
    if(count>65536) reject(data::ErrorCode::resource_limit,"profile point limit exceeded");
    // Ear clipping rebuilds reflex lists per ear. Bound its cubic worst case
    // before any backend allocation; checkpoints surround each bounded stage.
    usage.operations+=count*count*count;
    usage.working_bytes=std::max(usage.working_bytes,usage.vertices*32+count*256);
    checked(monitor.checkpoint(usage));
    poly_tri::Profile p;
    auto contour=[&](const data::Value::Array &input) {
        require(input.size()>=3,"profile contour needs at least three points");
        poly_tri::Contour c;
        c.reserve(input.size());
        for(const auto &v:input) {
            const auto &a=array(v);
            require(a.size()==2,"profile point must have two components");
            c.push_back({number(a[0]),number(a[1])});
        }
        require(std::abs(area(c))>1e-12,"profile contour has zero area");
        for(size_t i=0;i<c.size();++i) {
            size_t next=(i+1)%c.size();
            require(std::abs(double(c[i].x)-c[next].x)>=1e-6 ||
                    std::abs(double(c[i].y)-c[next].y)>=1e-6,"profile has coincident consecutive points");
            for(size_t j=i+1;j<c.size();++j) {
                if(j==next || (j+1)%c.size()==i) continue;
                require(!intersects(c[i],c[next],c[j],c[(j+1)%c.size()]),"profile contour self-intersects");
            }
        }
        // Adjacent edges must not double back along the same line.
        for(size_t i=0;i<c.size();++i) {
            auto a=c[(i+c.size()-1)%c.size()], b=c[i], d=c[(i+1)%c.size()];
            require(!(orient(a,b,d)==0 && (on(a,b,d)||on(b,d,a))),"profile has overlapping adjacent edges");
        }
        checked(monitor.checkpoint());
        return c;
    };
    p.outer=contour(outer);
    if(holes) for(const auto &h:*holes) p.holes.push_back(contour(array(h)));
    for(size_t i=0;i<p.holes.size();++i) {
        const auto &h=p.holes[i];
        require(inside(p.outer,h[0]),"profile hole is outside outer contour");
        auto no_cross=[&](const poly_tri::Contour &a,const poly_tri::Contour &b) {
            for(size_t j=0;j<a.size();++j) for(size_t k=0;k<b.size();++k)
                require(!intersects(a[j],a[(j+1)%a.size()],b[k],b[(k+1)%b.size()]),"profile contours touch or intersect");
        };
        no_cross(p.outer,h);
        for(size_t j=0;j<i;++j) {
            no_cross(p.holes[j],h);
            require(!inside(p.holes[j],h[0])&&!inside(h,p.holes[j][0]),"nested or overlapping holes are unsupported");
        }
    }
    CheckedProfile result;
    result.points=p.outer;
    for(const auto &h:p.holes) result.points.insert(result.points.end(),h.begin(),h.end());
    result.indices=poly_tri::triangulate(p);
    uint64_t expected=count+2*p.holes.size()-2;
    require(result.indices.size()==expected*3,"profile triangulation did not produce complete caps");
    double filled=std::abs(area(p.outer)), actual=0;
    for(const auto &h:p.holes) filled-=std::abs(area(h));
    for(size_t i=0;i<result.indices.size();i+=3) {
        for(size_t k=0;k<3;++k) require(result.indices[i+k]>=0 && uint64_t(result.indices[i+k])<count,"triangulator index outside profile");
        double a=orient(result.points[result.indices[i]],result.points[result.indices[i+1]],result.points[result.indices[i+2]])/2;
        require(a>0,"degenerate or reversed profile cap");
        actual+=a;
    }
    require(filled>0 && std::abs(actual-filled)<=1e-6*std::max(1.0,filled),"triangulation area does not cover the profile");
    // Walls use the emitter's winding verbatim; normalize a private copy.
    if(area(p.outer)<0) std::reverse(p.outer.begin(),p.outer.end());
    for(auto &h:p.holes) if(area(h)>0) std::reverse(h.begin(),h.end());
    for(auto v:p.outer) result.emit.outer.push_back({v.x,v.y});
    for(const auto &h:p.holes) {
        result.emit.holes.emplace_back();
        for(auto v:h) result.emit.holes.back().push_back({v.x,v.y});
    }
    checked(monitor.checkpoint());
    return result;
}
} // namespace matter::geometry::detail
