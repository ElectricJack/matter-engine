#include "internal.h"
#include <limits>
#include <map>
#include <memory>

// Batched closest-surface and ray-surface queries over evaluated prototypes.
// Each referenced prototype gets one bounded median-split BVH, built lazily and
// shared by every sample in the batch; samples never call back into a host.
namespace matter::geometry {
using namespace detail;
namespace {
constexpr uint32_t leaf_triangles=4, stack_limit=64, samples_per_checkpoint=64, visits_per_checkpoint=4096;
// Order, centroid, triangle box and at most two nodes per triangle.
constexpr uint64_t index_bytes_per_triangle=128, index_overhead_bytes=4096;
constexpr double barycentric_epsilon=1e-9, parallel_epsilon=1e-12;

struct D3 { double x=0,y=0,z=0; };
D3 d3(data::Vec3 v) { return {v.x,v.y,v.z}; }
data::Vec3 f3(D3 v) { return {float(v.x),float(v.y),float(v.z)}; }
D3 add(D3 a,D3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
D3 sub(D3 a,D3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
D3 mul(D3 a,double s) { return {a.x*s,a.y*s,a.z*s}; }
double dot(D3 a,D3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
D3 cross(D3 a,D3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
double axis(D3 v,int k) { return k==0?v.x:k==1?v.y:v.z; }
float component(data::Vec3 v,int k) { return k==0?v.x:k==1?v.y:v.z; }

struct Node { float lo[3], hi[3]; uint32_t first=0, count=0; }; // count==0: children at first, first+1.
struct Index {
    std::vector<Node> nodes;
    std::vector<uint32_t> order;
    double pad=0;
    bool built=false;
};
struct Corners { D3 a,b,c; };
Corners corners(const data::MeshPrototype &m,uint32_t tri) {
    return {d3(m.positions[m.indices[tri*3]]),d3(m.positions[m.indices[tri*3+1]]),d3(m.positions[m.indices[tri*3+2]])};
}

class Queries {
public:
    Queries(jobs::JobMonitor &monitor,jobs::ResourceUsage &usage) : monitor_(monitor), usage_(usage) {}
    void build(Index &index,const data::MeshPrototype &mesh) {
        index.built=true;
        const uint32_t count=uint32_t(mesh.indices.size()/3);
        if(!count) return;
        // Memory is admitted up front; scanned triangles, partitions and node
        // visits are charged as operations while the work runs.
        usage_.working_bytes+=uint64_t(count)*index_bytes_per_triangle+index_overhead_bytes;
        checked(monitor_.checkpoint(usage_));
        const auto &b=mesh.bounds;
        double extent=std::max({std::abs(b.min.x),std::abs(b.min.y),std::abs(b.min.z),
            std::abs(b.max.x),std::abs(b.max.y),std::abs(b.max.z)});
        index.pad=1e-6*(1+extent);
        std::vector<float> centroid(size_t(count)*3), lo(size_t(count)*3), hi(size_t(count)*3);
        index.order.resize(count);
        for(uint32_t t=0;t<count;++t) {
            visit(1);
            index.order[t]=t;
            const data::Vec3 p[3]={mesh.positions[mesh.indices[t*3]],mesh.positions[mesh.indices[t*3+1]],mesh.positions[mesh.indices[t*3+2]]};
            for(int k=0;k<3;++k) {
                float a=component(p[0],k), c=component(p[1],k), e=component(p[2],k);
                lo[t*3+k]=std::min({a,c,e}); hi[t*3+k]=std::max({a,c,e});
                centroid[t*3+k]=a+c+e;
            }
        }
        struct Task { uint32_t node, first, count; };
        std::vector<Task> tasks; tasks.reserve(stack_limit);
        index.nodes.reserve(size_t(count)*2);
        index.nodes.emplace_back();
        tasks.push_back({0,0,count});
        while(!tasks.empty()) {
            Task task=tasks.back(); tasks.pop_back();
            Node node;
            float clo[3], chi[3];
            for(int k=0;k<3;++k) {
                node.lo[k]=clo[k]=std::numeric_limits<float>::max();
                node.hi[k]=chi[k]=std::numeric_limits<float>::lowest();
            }
            for(uint32_t i=task.first;i<task.first+task.count;++i) {
                visit(1);
                uint32_t t=index.order[i];
                for(int k=0;k<3;++k) {
                    node.lo[k]=std::min(node.lo[k],lo[t*3+k]); node.hi[k]=std::max(node.hi[k],hi[t*3+k]);
                    clo[k]=std::min(clo[k],centroid[t*3+k]); chi[k]=std::max(chi[k],centroid[t*3+k]);
                }
            }
            if(task.count<=leaf_triangles) {
                node.first=task.first; node.count=task.count;
                index.nodes[task.node]=node;
                continue;
            }
            int split=0;
            for(int k=1;k<3;++k) if(chi[k]-clo[k]>chi[split]-clo[split]) split=k;
            // Total order: a zero-extent axis still halves the range.
            auto first=index.order.begin()+task.first;
            uint32_t half=task.count/2;
            visit(task.count); // The partition itself is linear and uninterruptible.
            std::nth_element(first,first+half,first+task.count,[&](uint32_t a,uint32_t c) {
                float ca=centroid[a*3+split], cc=centroid[c*3+split];
                return ca<cc || (ca==cc && a<c);
            });
            node.first=uint32_t(index.nodes.size()); node.count=0;
            index.nodes[task.node]=node;
            index.nodes.emplace_back(); index.nodes.emplace_back();
            if(tasks.size()+2>stack_limit) reject(data::ErrorCode::backend_failure,"surface index build stack exhausted");
            tasks.push_back({node.first+1,task.first+half,task.count-half});
            tasks.push_back({node.first,task.first,half});
        }
    }
    jobs::SurfaceSample ray(const Index &index,const data::MeshPrototype &mesh,const jobs::SurfaceQuery &q) {
        jobs::SurfaceSample out;
        if(index.nodes.empty()) return out;
        D3 origin=d3(q.position), direction=d3(q.direction);
        double length=std::sqrt(dot(direction,direction));
        // Validation rejects zero directions; overflowing magnitudes are explicit.
        if(!std::isfinite(length) || length<=0) reject(data::ErrorCode::invalid_input,"invalid surface query");
        direction=mul(direction,1/length);
        const double limit=q.max_distance_metres;
        double best_t=std::numeric_limits<double>::infinity(), best_u=0, best_v=0;
        uint32_t best=UINT32_MAX;
        uint32_t stack[stack_limit]; uint32_t size=0;
        stack[size++]=0;
        while(size) {
            const Node &node=index.nodes[stack[--size]];
            visit(1);
            double enter=0, exit=limit;
            if(!slab(node,index.pad,origin,direction,enter,exit) || enter>best_t) continue;
            if(node.count) {
                for(uint32_t i=node.first;i<node.first+node.count;++i) {
                    uint32_t t=index.order[i];
                    auto c=corners(mesh,t);
                    D3 e1=sub(c.b,c.a), e2=sub(c.c,c.a), n=cross(e1,e2);
                    D3 p=cross(direction,e2);
                    double det=dot(e1,p), area=std::sqrt(dot(n,n));
                    if(!(std::abs(det)>parallel_epsilon*area)) continue;
                    double inv=1/det;
                    D3 s=sub(origin,c.a);
                    double u=dot(s,p)*inv;
                    if(u< -barycentric_epsilon || u>1+barycentric_epsilon) continue;
                    D3 r=cross(s,e1);
                    double v=dot(direction,r)*inv;
                    if(v< -barycentric_epsilon || u+v>1+barycentric_epsilon) continue;
                    double hit=dot(e2,r)*inv;
                    if(!(hit>=0 && hit<=limit)) continue;
                    if(hit<best_t || (hit==best_t && t<best)) { best_t=hit; best=t; best_u=u; best_v=v; }
                }
            } else {
                if(size+2>stack_limit) reject(data::ErrorCode::backend_failure,"surface query traversal stack exhausted");
                stack[size++]=node.first+1; stack[size++]=node.first;
            }
        }
        if(best==UINT32_MAX) return out;
        best_u=std::max(best_u,0.0); best_v=std::max(best_v,0.0);
        if(best_u+best_v>1) { double sum=best_u+best_v; best_u/=sum; best_v/=sum; }
        return sample(mesh,best,1-best_u-best_v,best_u,best_v,best_t);
    }
    jobs::SurfaceSample closest(const Index &index,const data::MeshPrototype &mesh,const jobs::SurfaceQuery &q) {
        jobs::SurfaceSample out;
        if(index.nodes.empty()) return out;
        D3 point=d3(q.position);
        const double limit=double(q.max_distance_metres)*double(q.max_distance_metres);
        double best_d=std::numeric_limits<double>::infinity();
        D3 best_w;
        uint32_t best=UINT32_MAX;
        struct Entry { uint32_t node; double distance; };
        Entry stack[stack_limit]; uint32_t size=0;
        stack[size++]={0,box_distance(index.nodes[0],index.pad,point)};
        while(size) {
            Entry entry=stack[--size];
            visit(1);
            double bound=std::min(best_d,limit);
            // Tolerance keeps equal-distance candidates for the index tie-break.
            if(entry.distance>bound*(1+1e-9)+1e-18) continue;
            const Node &node=index.nodes[entry.node];
            if(node.count) {
                for(uint32_t i=node.first;i<node.first+node.count;++i) {
                    uint32_t t=index.order[i];
                    auto c=corners(mesh,t);
                    D3 w=closest_weights(point,c);
                    D3 at=add(add(mul(c.a,w.x),mul(c.b,w.y)),mul(c.c,w.z));
                    D3 delta=sub(point,at);
                    double d=dot(delta,delta);
                    if(d>limit) continue;
                    if(d<best_d || (d==best_d && t<best)) { best_d=d; best=t; best_w=w; }
                }
            } else {
                if(size+2>stack_limit) reject(data::ErrorCode::backend_failure,"surface query traversal stack exhausted");
                Entry left{node.first,box_distance(index.nodes[node.first],index.pad,point)};
                Entry right{node.first+1,box_distance(index.nodes[node.first+1],index.pad,point)};
                if(left.distance<=right.distance) { stack[size++]=right; stack[size++]=left; }
                else { stack[size++]=left; stack[size++]=right; }
            }
        }
        if(best==UINT32_MAX) return out;
        return sample(mesh,best,best_w.x,best_w.y,best_w.z,std::sqrt(best_d));
    }
private:
    jobs::JobMonitor &monitor_;
    jobs::ResourceUsage &usage_;
    uint32_t visits_=0;
    void visit(uint32_t n) {
        usage_.operations+=n;
        visits_+=n;
        if(visits_>=visits_per_checkpoint) { visits_=0; checked(monitor_.checkpoint(usage_)); }
    }
    static bool slab(const Node &node,double pad,D3 origin,D3 direction,double &enter,double &exit) {
        for(int k=0;k<3;++k) {
            double o=axis(origin,k), d=axis(direction,k);
            double lo=node.lo[k]-pad, hi=node.hi[k]+pad;
            if(d==0) {
                if(o<lo || o>hi) return false;
                continue;
            }
            double a=(lo-o)/d, b=(hi-o)/d;
            if(a>b) std::swap(a,b);
            enter=std::max(enter,a); exit=std::min(exit,b);
            if(enter>exit) return false;
        }
        return true;
    }
    static double box_distance(const Node &node,double pad,D3 p) {
        double sum=0;
        for(int k=0;k<3;++k) {
            double v=axis(p,k), lo=node.lo[k]-pad, hi=node.hi[k]+pad;
            double d=v<lo?lo-v:v>hi?v-hi:0;
            sum+=d*d;
        }
        return sum;
    }
    // Ericson, Real-Time Collision Detection 5.1.5, returning barycentric weights.
    static D3 closest_weights(D3 p,const Corners &c) {
        D3 ab=sub(c.b,c.a), ac=sub(c.c,c.a), ap=sub(p,c.a);
        double d1=dot(ab,ap), d2=dot(ac,ap);
        if(d1<=0 && d2<=0) return {1,0,0};
        D3 bp=sub(p,c.b);
        double d3v=dot(ab,bp), d4=dot(ac,bp);
        if(d3v>=0 && d4<=d3v) return {0,1,0};
        double vc=d1*d4-d3v*d2;
        if(vc<=0 && d1>=0 && d3v<=0) { double v=d1/(d1-d3v); return {1-v,v,0}; }
        D3 cp=sub(p,c.c);
        double d5=dot(ab,cp), d6=dot(ac,cp);
        if(d6>=0 && d5<=d6) return {0,0,1};
        double vb=d5*d2-d1*d6;
        if(vb<=0 && d2>=0 && d6<=0) { double w=d2/(d2-d6); return {1-w,0,w}; }
        double va=d3v*d6-d5*d4;
        if(va<=0 && d4-d3v>=0 && d5-d6>=0) { double w=(d4-d3v)/((d4-d3v)+(d5-d6)); return {0,1-w,w}; }
        double inv=1/(va+vb+vc), v=vb*inv, w=vc*inv;
        return {1-v-w,v,w};
    }
    static jobs::SurfaceSample sample(const data::MeshPrototype &mesh,uint32_t tri,double wa,double wb,double wc,double distance) {
        auto c=corners(mesh,tri);
        jobs::SurfaceSample out;
        out.hit=true; out.triangle=tri; out.distance_metres=float(distance);
        out.position=f3(add(add(mul(c.a,wa),mul(c.b,wb)),mul(c.c,wc)));
        D3 n{};
        if(!mesh.normals.empty())
            n=add(add(mul(d3(mesh.normals[mesh.indices[tri*3]]),wa),mul(d3(mesh.normals[mesh.indices[tri*3+1]]),wb)),
                mul(d3(mesh.normals[mesh.indices[tri*3+2]]),wc));
        double length=std::sqrt(dot(n,n));
        if(!(length>1e-12) || !std::isfinite(length)) {
            // Authored normals cancelled or absent: geometric normal of the
            // v1 counterclockwise winding. Validation rejected zero-area triangles.
            n=cross(sub(c.b,c.a),sub(c.c,c.a));
            length=std::sqrt(dot(n,n));
        }
        out.normal=f3(mul(n,1/length));
        return out;
    }
};
}

jobs::QueryResult PrimitiveGeometry::query(const jobs::JobEnvelope &job,const jobs::AssetProduct &product,
    const jobs::QueryBatch &batch,const jobs::EvaluationOptions &options,const jobs::JobControl &control) {
    try {
        const jobs::JobEnvelope admitted_job=job;
        auto bounded=options;
        bounded.validation.supported_capabilities=capabilities();
        auto r=jobs::validate(admitted_job);
        if(r.ok()) r=jobs::validate(options.budget);
        if(r.ok()) r=jobs::require_capabilities(options.algorithms,bounded.validation.supported_capabilities);
        if(r.ok()) r=jobs::validate(product,bounded.validation);
        if(r.ok()) r=jobs::validate(batch,product.document,bounded);
        if(!r.ok()) return jobs::QueryResult::failure(job,std::move(r.diagnostics));
        for(const auto &extension:product.document.extensions)
            if(extension.required)
                reject(data::ErrorCode::unsupported_capability,"geometry queries do not implement document extensions");
        jobs::JobMonitor monitor(options.budget,control);
        jobs::ResourceUsage usage;
        checked(monitor.checkpoint());
        checked(monitor.report({0,"validate"}));
        const auto &meshes=product.document.meshes;
        const size_t count=batch.samples.size();
        usage.queries=count;
        usage.output_bytes=uint64_t(count)*sizeof(jobs::SurfaceSample);
        usage.working_bytes=usage.output_bytes+uint64_t(meshes.size())*sizeof(Index);
        checked(monitor.checkpoint(usage));
        std::map<std::string,size_t,std::less<>> prototypes;
        for(size_t i=0;i<meshes.size();++i) prototypes.emplace(meshes[i].id.value,i);
        std::vector<Index> indices(meshes.size());
        Queries queries(monitor,usage);
        jobs::QueryProduct out;
        out.samples.reserve(count);
        for(size_t i=0;i<count;++i) {
            const auto &q=batch.samples[i];
            size_t m=prototypes.at(q.prototype.value);
            if(!indices[m].built) queries.build(indices[m],meshes[m]);
            out.samples.push_back(q.kind==jobs::QueryKind::ray_surface ? queries.ray(indices[m],meshes[m],q) :
                queries.closest(indices[m],meshes[m],q));
            ++usage.operations;
            if((i+1)%samples_per_checkpoint==0) checked(monitor.checkpoint(usage));
        }
        checked(monitor.checkpoint(usage));
        checked(monitor.report({1,"complete"}));
        return published(jobs::QueryResult::success(admitted_job,std::move(out)),admitted_job,
            "geometry query publication allocation failed");
    } catch(const Failure &f) {
        return jobs::QueryResult::failure(job,std::vector<data::Diagnostic>{{data::Severity::error,f.code,f.message,f.node,{},{}}});
    } catch(const std::bad_alloc &) {
        return jobs::QueryResult::failure(job,data::ErrorCode::resource_limit,"geometry query allocation failed");
    } catch(const std::exception &e) {
        return jobs::QueryResult::failure(job,data::ErrorCode::backend_failure,e.what());
    } catch(...) {
        return jobs::QueryResult::failure(job,data::ErrorCode::backend_failure,"geometry query backend exception");
    }
}
} // namespace matter::geometry
