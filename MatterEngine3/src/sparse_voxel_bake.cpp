#include "sparse_voxel_bake.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace sparse_voxel {
namespace {
constexpr double diagonal=0.7071067811865475244;
constexpr std::array<std::array<double,3>,9> projection_directions{{
    {1,0,0},{0,1,0},{0,0,1},{diagonal,diagonal,0},{diagonal,-diagonal,0},
    {diagonal,0,diagonal},{diagonal,0,-diagonal},{0,diagonal,diagonal},{0,diagonal,-diagonal}}};
bool finite(mm::Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
double component(mm::Vec3 v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }
bool valid_sample(const SurfaceSample& s) {
    return finite(s.albedo) && s.albedo.x >= 0 && s.albedo.y >= 0 && s.albedo.z >= 0 &&
           std::isfinite(s.coverage) && s.coverage >= 0 && s.coverage <= 1;
}
int32_t floor_div(int32_t x, int32_t divisor) {
    const int32_t q = x / divisor, r = x % divisor;
    return q - (r < 0 ? 1 : 0);
}
void accumulate(Cell& a, const Cell& b) {
    if(a.area==0) {a.has_projection=b.has_projection;a.projected_area=b.projected_area;}
    else if(a.has_projection && b.has_projection)
        for(int k=0;k<9;++k) a.projected_area[k]+=b.projected_area[k];
    else {a.has_projection=false;a.projected_area={};}
    if(a.area==0) {
        a.has_support=b.has_support;a.support_min=b.support_min;a.support_max=b.support_max;a.plane=b.plane;
    } else if(a.has_support && b.has_support) {
        double alignment=0;for(int k=0;k<3;++k) alignment+=a.plane[k]*b.plane[k];
        double tolerance=1e-12;
        for(int k=0;k<3;++k) {
            a.support_min[k]=std::min(a.support_min[k],b.support_min[k]);
            a.support_max[k]=std::max(a.support_max[k],b.support_max[k]);
            tolerance=std::max(tolerance,1e-8*(a.support_max[k]-a.support_min[k]));
        }
        if(alignment<1-1e-10 || std::abs(a.plane[3]-b.plane[3])>tolerance) a.plane={};
    } else {a.has_support=false;a.plane={};}
    a.area += b.area;
    for (int k = 0; k < 3; ++k) {
        a.albedo_area[k] += b.albedo_area[k]; a.normal_area[k] += b.normal_area[k];
    }
    for (int k = 0; k < 6; ++k) a.normal_second_area[k] += b.normal_second_area[k];
}
uint32_t bit_count(uint64_t mask) {
    uint32_t count = 0;
    for (; mask; mask &= mask - 1) ++count;
    return count;
}
void pack(const std::map<Coord, Cell>& cells, mm::Vec3 origin, float size, Asset& out) {
    struct Key {
        Coord brick; uint32_t bit;
        bool operator<(const Key& b) const { return brick != b.brick ? brick < b.brick : bit < b.bit; }
    };
    std::map<Key, const Cell*> ordered;
    for (const auto& [coord, cell] : cells) {
        Coord brick{}; uint32_t local[3]{};
        for (int k = 0; k < 3; ++k) {
            brick[k] = floor_div(coord[k], 4);
            local[k] = uint32_t(int64_t(coord[k]) - int64_t(brick[k]) * 4);
        }
        ordered.emplace(Key{brick, local[0] + 4*local[1] + 16*local[2]}, &cell);
    }
    Asset result; result.origin = origin; result.cell_size = size;
    result.cells.reserve(cells.size());
    for (const auto& [key, cell] : ordered) {
        if (result.bricks.empty() || result.bricks.back().coord != key.brick)
            result.bricks.push_back({key.brick, 0, uint32_t(result.cells.size())});
        result.bricks.back().mask |= uint64_t(1) << key.bit;
        result.cells.push_back(*cell);
    }
    out = std::move(result);
}

// Double-precision clipping avoids inflating thin geometry into full cells.
// Barycentric weights travel through clipping so texture coordinates continue
// to refer to the original surface, including on cell/brick boundaries.
struct Vertex { double p[3], bary[3]; };
using Polygon = std::vector<Vertex>;
Polygon clip(const Polygon& input, int axis, double plane, bool greater) {
    Polygon out; out.reserve(input.size()+1);
    if (input.empty()) return out;
    Vertex previous = input.back();
    bool was_inside = greater ? previous.p[axis] >= plane : previous.p[axis] <= plane;
    for (const auto& current : input) {
        const bool inside = greater ? current.p[axis] >= plane : current.p[axis] <= plane;
        if (inside != was_inside) {
            const double t = (plane - previous.p[axis]) / (current.p[axis] - previous.p[axis]);
            Vertex intersection{};
            for (int k = 0; k < 3; ++k) {
                intersection.p[k] = previous.p[k] + t*(current.p[k]-previous.p[k]);
                intersection.bary[k] = previous.bary[k] + t*(current.bary[k]-previous.bary[k]);
            }
            intersection.p[axis] = plane;
            out.push_back(intersection);
        }
        if (inside) out.push_back(current);
        previous = current; was_inside = inside;
    }
    return out;
}
double area(const Vertex& a, const Vertex& b, const Vertex& c, double normal[3]) {
    double ab[3], ac[3];
    for (int k = 0; k < 3; ++k) { ab[k] = b.p[k]-a.p[k]; ac[k] = c.p[k]-a.p[k]; }
    normal[0] = ab[1]*ac[2]-ab[2]*ac[1];
    normal[1] = ab[2]*ac[0]-ab[0]*ac[2];
    normal[2] = ab[0]*ac[1]-ab[1]*ac[0];
    const double length = std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
    if (length > 0) for (int k = 0; k < 3; ++k) normal[k] /= length;
    return length * 0.5;
}
} // namespace

Builder::Builder(Config config) : config_(config) {
    if (!finite(config.origin) || !std::isfinite(config.cell_size) ||
        !(config.cell_size > 0) || !config.max_cells || !config.max_cell_tests)
        fail("sparse voxel bake: invalid configuration");
}
bool Builder::fail(const char* message) { error_ = message; return false; }

bool Builder::add(const Triangle& triangle, const SurfaceSampler& sampler) {
    if (!error_.empty()) return false;
    if (!valid_sample(triangle.surface)) return fail("sparse voxel bake: invalid surface");
    Polygon original(3);
    for (int v = 0; v < 3; ++v) {
        if (!finite(triangle.positions[v]) || !std::isfinite(triangle.uv[v].x) ||
            !std::isfinite(triangle.uv[v].y)) return fail("sparse voxel bake: nonfinite vertex");
        for (int k = 0; k < 3; ++k) {
            original[v].p[k] = component(triangle.positions[v], k);
            original[v].bary[k] = k == v ? 1.0 : 0.0;
        }
    }
    double normal[3];
    if (!(area(original[0], original[1], original[2], normal) > 0)) return true;
    if (triangle.surface.coverage == 0) return true;
    Coord lo{}, hi{};
    uint64_t tests = 1;
    for (int k = 0; k < 3; ++k) {
        const double mn = std::min({original[0].p[k],original[1].p[k],original[2].p[k]});
        const double mx = std::max({original[0].p[k],original[1].p[k],original[2].p[k]});
        const double low = std::floor((mn-component(config_.origin,k))/config_.cell_size);
        // Half-open ownership: a surface exactly on a grid plane belongs to
        // one cell. This prevents double area for coplanar cards on boundaries.
        const double high = mx == mn ? low : std::ceil((mx-component(config_.origin,k))/config_.cell_size)-1;
        if (low < INT32_MIN || high > INT32_MAX || low > high)
            return fail("sparse voxel bake: coordinate overflow");
        lo[k] = int32_t(low); hi[k] = int32_t(high);
        const uint64_t span = uint64_t(int64_t(hi[k])-lo[k]+1);
        if (tests > (config_.max_cell_tests-cell_tests_) / span)
            return fail("sparse voxel bake: cell-test budget exceeded");
        tests *= span;
    }
    cell_tests_ += tests;
    for (int64_t z = lo[2]; z <= hi[2]; ++z)
    for (int64_t y = lo[1]; y <= hi[1]; ++y)
    for (int64_t x = lo[0]; x <= hi[0]; ++x) {
        const Coord coord{int32_t(x),int32_t(y),int32_t(z)};
        Polygon polygon = original;
        for (int k = 0; k < 3 && !polygon.empty(); ++k) {
            const double lower = component(config_.origin,k) + double(coord[k])*config_.cell_size;
            polygon = clip(polygon, k, lower, true);
            polygon = clip(polygon, k, lower+config_.cell_size, false);
        }
        Cell contribution;
        for (size_t i = 1; i+1 < polygon.size(); ++i) {
            double n[3]; const double patch_area = area(polygon[0],polygon[i],polygon[i+1],n);
            if (!(patch_area > 0)) continue;
            SurfaceSample sample = triangle.surface;
            if (sampler) {
                SampleRequest request{};
                double p[3]{}, uv[2]{};
                for (int k = 0; k < 3; ++k) {
                    p[k] = (polygon[0].p[k]+polygon[i].p[k]+polygon[i+1].p[k])/3;
                    const double w = (polygon[0].bary[k]+polygon[i].bary[k]+polygon[i+1].bary[k])/3;
                    uv[0] += w*triangle.uv[k].x; uv[1] += w*triangle.uv[k].y;
                }
                request.position = {float(p[0]),float(p[1]),float(p[2])};
                request.uv = {float(uv[0]),float(uv[1])};
                request.footprint_m = float(std::sqrt(patch_area));
                const SurfaceSample texture = sampler(request);
                if (!valid_sample(texture)) return fail("sparse voxel bake: invalid sampled surface");
                sample.coverage *= texture.coverage;
                sample.albedo = {sample.albedo.x*texture.albedo.x,
                                 sample.albedo.y*texture.albedo.y, sample.albedo.z*texture.albedo.z};
            }
            const double weight = patch_area * sample.coverage;
            contribution.area += weight;
            for (int k = 0; k < 3; ++k) {
                contribution.albedo_area[k] += weight*component(sample.albedo,k);
                contribution.normal_area[k] += weight*n[k];
                contribution.normal_second_area[k] += weight*n[k]*n[k];
            }
            contribution.normal_second_area[3] += weight*n[0]*n[1];
            contribution.normal_second_area[4] += weight*n[0]*n[2];
            contribution.normal_second_area[5] += weight*n[1]*n[2];
        }
        if (!(contribution.area > 0)) continue;
        contribution.has_support=true;
        contribution.has_projection=true;
        for(int d=0;d<9;++d) {
            double projected=0;for(int k=0;k<3;++k) projected+=normal[k]*projection_directions[d][k];
            contribution.projected_area[d]=contribution.area*std::abs(projected);
        }
        contribution.support_min={polygon[0].p[0],polygon[0].p[1],polygon[0].p[2]};
        contribution.support_max=contribution.support_min;
        for(const auto& v:polygon) for(int k=0;k<3;++k) {
            contribution.support_min[k]=std::min(contribution.support_min[k],v.p[k]);
            contribution.support_max[k]=std::max(contribution.support_max[k],v.p[k]);
        }
        int dominant=0;for(int k=1;k<3;++k) if(std::abs(normal[k])>std::abs(normal[dominant])) dominant=k;
        const double sign=normal[dominant]<0?-1:1;
        for(int k=0;k<3;++k) {
            contribution.plane[k]=sign*normal[k];
            contribution.plane[3]+=sign*normal[k]*original[0].p[k];
        }
        auto found = cells_.find(coord);
        if (found == cells_.end()) {
            if (cells_.size() >= config_.max_cells) return fail("sparse voxel bake: occupied-cell budget exceeded");
            found = cells_.emplace(coord, Cell{}).first;
        }
        accumulate(found->second, contribution);
    }
    return true;
}

bool Builder::finish(Asset& out, std::string& error) const {
    error = error_;
    if (!error.empty()) return false;
    Asset candidate;
    pack(cells_, config_.origin, config_.cell_size, candidate);
    if (!validate(candidate,error)) return false;
    out=std::move(candidate);
    return true;
}
bool similarity_scale(const mm::Mat4& m, double& scale) {
    for (float v:m.m) if (!std::isfinite(v)) return false;
    if (m.m[12]!=0 || m.m[13]!=0 || m.m[14]!=0 || m.m[15]!=1) return false;
    double lengths[3]{};
    for (int i=0;i<3;++i) for(int k=0;k<3;++k) lengths[i]+=double(m.m[k*4+i])*m.m[k*4+i];
    const double mean=(lengths[0]+lengths[1]+lengths[2])/3;
    if (!(mean>0) || !std::isfinite(mean)) return false;
    // Authored float rotation chains accumulate several ULPs of drift.
    for (int i=0;i<3;++i) {
        if(std::abs(lengths[i]-mean)>mean*2e-5) return false;
        for(int j=i+1;j<3;++j) {
            double dot=0; for(int k=0;k<3;++k) dot+=double(m.m[k*4+i])*m.m[k*4+j];
            if(std::abs(dot)>mean*2e-5) return false;
        }
    }
    scale=std::sqrt(mean); return true;
}

bool Builder::add(const Asset& source,const mm::Mat4& transform) {
    if (!error_.empty()) return false;
    if (!validate(source,error_)) return false;
    double scale=0;
    if (!similarity_scale(transform,scale)) return fail("sparse voxel aggregate requires a similarity transform");
    if(source.cells.empty()) return true;
    const double width=double(source.cell_size)*scale/config_.cell_size;
    if (!std::isfinite(width) || width>2.00001)
        return fail("sparse voxel aggregate source cells exceed twice the destination spacing");
    // At most 4^3 quadrature points per occupied source cell, with spacing
    // <= half a destination cell. Never expand the placed source triangles.
    const uint32_t n=uint32_t(std::clamp(std::ceil(2*width),1.0,4.0));
    const uint64_t samples=uint64_t(n)*n*n;
    if (source.cells.size()>(config_.max_cell_tests-cell_tests_)/samples)
        return fail("sparse voxel aggregate exceeds cell work budget");
    double R[3][3];
    for (int i=0;i<3;++i) for(int j=0;j<3;++j) R[i][j]=transform.m[i*4+j]/scale;
    const double determinant=R[0][0]*(R[1][1]*R[2][2]-R[1][2]*R[2][1])-
        R[0][1]*(R[1][0]*R[2][2]-R[1][2]*R[2][0])+R[0][2]*(R[1][0]*R[2][1]-R[1][1]*R[2][0]);
    const double sign=determinant<0?-1:1;
    const double factor=scale*scale/samples;
    for(const auto& brick:source.bricks) {
        size_t offset=brick.first_cell;
        for(uint32_t bit=0;bit<64;++bit) if(brick.mask&(uint64_t(1)<<bit)) {
            const Cell& input=source.cells[offset++];
            Cell contribution; contribution.area=input.area*factor;
            contribution.has_projection=true;
            const auto projected=projected_area_matrix(input);
            for(int d=0;d<9;++d) {
                double p[3]{};
                for(int i=0;i<3;++i) for(int j=0;j<3;++j) p[i]+=R[j][i]*projection_directions[d][j];
                const double square=projected[0]*p[0]*p[0]+projected[1]*p[1]*p[1]+projected[2]*p[2]*p[2]+
                    2*(projected[3]*p[0]*p[1]+projected[4]*p[0]*p[2]+projected[5]*p[1]*p[2]);
                contribution.projected_area[d]=contribution.area*std::sqrt(std::max(0.0,square));
            }
            const double M[3][3]={{input.normal_second_area[0],input.normal_second_area[3],input.normal_second_area[4]},
                {input.normal_second_area[3],input.normal_second_area[1],input.normal_second_area[5]},
                {input.normal_second_area[4],input.normal_second_area[5],input.normal_second_area[2]}};
            double rotated[3][3]{};
            for(int i=0;i<3;++i) {
                contribution.albedo_area[i]=input.albedo_area[i]*factor;
                for(int j=0;j<3;++j) {
                    contribution.normal_area[i]+=sign*factor*R[i][j]*input.normal_area[j];
                    for(int k=0;k<3;++k) for(int l=0;l<3;++l)
                        rotated[i][j]+=factor*R[i][k]*M[k][l]*R[j][l];
                }
            }
            contribution.normal_second_area={rotated[0][0],rotated[1][1],rotated[2][2],rotated[0][1],rotated[0][2],rotated[1][2]};
            const int local[3]={int(bit%4),int((bit/4)%4),int(bit/16)};
            double lower[3];
            for(int k=0;k<3;++k) lower[k]=component(source.origin,k)+
                (int64_t(brick.coord[k])*4+local[k])*double(source.cell_size);
            double widths[3]={source.cell_size,source.cell_size,source.cell_size};
            if(input.has_support) {
                contribution.has_support=true;
                contribution.support_min={1e300,1e300,1e300};contribution.support_max={-1e300,-1e300,-1e300};
                for(int k=0;k<3;++k) {lower[k]=input.support_min[k];widths[k]=input.support_max[k]-input.support_min[k];}
                for(int corner=0;corner<8;++corner) for(int i=0;i<3;++i) {
                    double value=transform.m[i*4+3];
                    for(int j=0;j<3;++j) value+=transform.m[i*4+j]*(lower[j]+((corner>>j)&1)*widths[j]);
                    contribution.support_min[i]=std::min(contribution.support_min[i],value);
                    contribution.support_max[i]=std::max(contribution.support_max[i],value);
                }
                // This path resamples a volume quadrature. Retain conservative
                // support but do not label those samples as an exact plane.
            }
            for(uint32_t z=0;z<n;++z) for(uint32_t y=0;y<n;++y) for(uint32_t x=0;x<n;++x) {
                const double p[3]={lower[0]+(x+0.5)*widths[0]/n,
                    lower[1]+(y+0.5)*widths[1]/n,lower[2]+(z+0.5)*widths[2]/n};
                Coord coord{};
                for(int i=0;i<3;++i) {
                    double value=transform.m[i*4+3];
                    for(int j=0;j<3;++j) value+=transform.m[i*4+j]*p[j];
                    const double cell=std::floor((value-component(config_.origin,i))/config_.cell_size);
                    if (!std::isfinite(cell) || cell<INT32_MIN || cell>INT32_MAX)
                        return fail("sparse voxel aggregate coordinate overflow");
                    coord[i]=int32_t(cell);
                }
                ++cell_tests_;
                auto it=cells_.find(coord);
                if(it==cells_.end()) {
                    if(cells_.size()>=config_.max_cells) return fail("sparse voxel aggregate exceeds occupied cell budget");
                    it=cells_.emplace(coord,Cell{}).first;
                }
                Cell clipped=contribution;
                if(clipped.has_support) for(int k=0;k<3;++k) {
                    const double lo=component(config_.origin,k)+double(coord[k])*config_.cell_size;
                    clipped.support_min[k]=std::max(clipped.support_min[k],lo);
                    clipped.support_max[k]=std::min(clipped.support_max[k],lo+config_.cell_size);
                }
                accumulate(it->second,clipped);
            }
        }
    }
    return true;
}

bool validate(const Asset& source, std::string& error) {
    const auto fail = [&](const char* msg) { error = msg; return false; };
    if (!finite(source.origin) || !(source.cell_size > 0) || !std::isfinite(source.cell_size))
        return fail("sparse voxel asset: invalid grid");
    size_t offset = 0;
    for (size_t i = 0; i < source.bricks.size(); ++i) {
        const auto& b = source.bricks[i];
        if (!b.mask || b.first_cell != offset ||
            (i && !(source.bricks[i-1].coord < b.coord))) return fail("sparse voxel asset: invalid brick table");
        offset += bit_count(b.mask);
        if (offset > source.cells.size()) return fail("sparse voxel asset: cell range overflow");
    }
    if (offset != source.cells.size()) return fail("sparse voxel asset: orphan cells");
    for (const auto& c : source.cells) {
        if (!(c.area > 0) || !std::isfinite(c.area)) return fail("sparse voxel asset: invalid area");
        for (double v : c.albedo_area) if (!std::isfinite(v) || v < 0) return fail("sparse voxel asset: invalid color");
        for (double v : c.normal_area) if (!std::isfinite(v)) return fail("sparse voxel asset: invalid normal");
        for (double v : c.normal_second_area) if (!std::isfinite(v)) return fail("sparse voxel asset: invalid moments");
        if(c.has_support) for(int k=0;k<3;++k)
            if(!std::isfinite(c.support_min[k]) || !std::isfinite(c.support_max[k]) || c.support_min[k]>c.support_max[k])
                return fail("sparse voxel asset: invalid surface support");
        double norm=0;for(int k=0;k<4;++k) {
            if(!std::isfinite(c.plane[k])) return fail("sparse voxel asset: nonfinite support plane");
            if(k<3) norm+=c.plane[k]*c.plane[k];
        }
        if(norm!=0 && (!c.has_support || std::abs(norm-1)>1e-8)) return fail("sparse voxel asset: invalid support plane");
        if(c.has_projection) for(double p:c.projected_area)
            if(!std::isfinite(p) || p<0 || p>c.area*(1+1e-6)) return fail("sparse voxel asset: invalid projected area");
    }
    error.clear(); return true;
}
double support_plane_area(const Cell& c) {
    if(!c.has_support) return 0;
    int axis=0;for(int k=1;k<3;++k) if(std::abs(c.plane[k])>std::abs(c.plane[axis])) axis=k;
    if(std::abs(c.plane[axis])<0.5) return 0;
    const int u=(axis+1)%3,v=(axis+2)%3;
    Polygon polygon;
    for(int corner:{0,1,3,2}) {
        Vertex p{};p.p[u]=(corner&1)?c.support_max[u]:c.support_min[u];
        p.p[v]=(corner&2)?c.support_max[v]:c.support_min[v];
        p.p[axis]=(c.plane[3]-c.plane[u]*p.p[u]-c.plane[v]*p.p[v])/c.plane[axis];
        polygon.push_back(p);
    }
    // Tolerance protects a plane exactly coincident with a support face from
    // roundoff in its reconstructed equation; it does not expand the polygon.
    const double eps=1e-10*std::max({c.support_max[0]-c.support_min[0],c.support_max[1]-c.support_min[1],c.support_max[2]-c.support_min[2],1e-6});
    polygon=clip(polygon,axis,c.support_min[axis]-eps,true);
    polygon=clip(polygon,axis,c.support_max[axis]+eps,false);
    double result=0;
    for(size_t i=1;i+1<polygon.size();++i) {double normal[3];result+=area(polygon[0],polygon[i],polygon[i+1],normal);}
    return result;
}
std::array<double,6> projected_area_matrix(const Cell& c) {
    std::array<double,6> result{};
    if(!c.has_projection) {for(int k=0;k<6;++k) result[k]=c.normal_second_area[k]/c.area;return result;}
    double square[9];for(int k=0;k<9;++k) {const double a=c.projected_area[k]/c.area;square[k]=a*a;}
    result={square[0],square[1],square[2],0.5*(square[3]-square[4]),0.5*(square[5]-square[6]),0.5*(square[7]-square[8])};
    // A single ellipsoid cannot fit every arbitrary normal distribution. Keep
    // the measured axis projections and contract off-diagonal terms only when
    // needed to make this bounded fit positive semidefinite.
    const auto valid=[&](double scale) {
        const double x=result[3]*scale,y=result[4]*scale,z=result[5]*scale;
        return result[0]*result[1]-x*x>=-1e-14 && result[0]*result[2]-y*y>=-1e-14 &&
            result[1]*result[2]-z*z>=-1e-14 &&
            result[0]*result[1]*result[2]+2*x*y*z-result[0]*z*z-result[1]*y*y-result[2]*x*x>=-1e-14;
    };
    if(!valid(1)) {
        double low=0,high=1;for(int i=0;i<32;++i) {const double middle=(low+high)*0.5;if(valid(middle)) low=middle;else high=middle;}
        for(int k=3;k<6;++k) result[k]*=low;
    }
    return result;
}
bool coarsen(const Asset& source, Asset& out, std::string& error) {
    if (!validate(source, error)) return false;
    if (!std::isfinite(source.cell_size*2)) { error = "sparse voxel asset: parent grid overflow"; return false; }
    std::map<Coord, Cell> cells;
    for (const auto& b : source.bricks) {
        size_t offset = b.first_cell;
        for (uint32_t bit = 0; bit < brick_cells; ++bit) if (b.mask & (uint64_t(1)<<bit)) {
            Coord parent{};
            const int local[3] = {int(bit%4),int((bit/4)%4),int(bit/16)};
            for (int k = 0; k < 3; ++k) {
                const int64_t child = int64_t(b.coord[k])*4 + local[k];
                if (child < INT32_MIN || child > INT32_MAX) {
                    error = "sparse voxel asset: brick coordinate overflow"; return false;
                }
                parent[k] = floor_div(int32_t(child), 2);
            }
            accumulate(cells[parent], source.cells[offset++]);
        }
    }
    pack(cells, source.origin, source.cell_size*2, out);
    error.clear(); return true;
}
} // namespace sparse_voxel
