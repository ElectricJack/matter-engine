#include "csg.h"
#include "shared_surface_math.h"
#include <climits>

namespace matter::geometry::detail {
namespace {
int operation(const data::Value &v) {
    auto s=std::get_if<std::string>(&v.data);
    require(s!=nullptr,"CSG operator must be a string");
    if(*s=="union") return 0;
    if(*s=="subtract") return 1;
    if(*s=="intersect") return 2;
    reject(data::ErrorCode::invalid_input,"unknown CSG operator");
}
float fold(float a,float b,int op) {
    return op==0?std::min(a,b):op==1?std::max(a,-b):std::max(a,b);
}
void keys(const data::RecipeNode &n,std::initializer_list<const char *> allowed) {
    auto p=std::get_if<data::Value::Object>(&n.parameters.data);
    require(p!=nullptr,"CSG parameters must be an object");
    for(const auto &entry:*p) {
        bool found=false;
        for(auto key:allowed) if(entry.first==key) found=true;
        require(found,"unknown CSG operand parameter");
    }
}
void tick(jobs::JobMonitor &monitor,jobs::ResourceUsage &usage) {
    if(usage.operations==UINT64_MAX) reject(data::ErrorCode::resource_limit,"CSG operation counter overflow");
    ++usage.operations;
    if((usage.operations&127)==0) checked(monitor.checkpoint(usage));
}
}

CsgField lower_csg(const data::RecipeNode &root,
    const std::map<std::string,const data::RecipeNode *> &nodes,CsgMeshingSettings settings,
    jobs::JobMonitor &monitor,jobs::ResourceUsage &usage) {
    CsgField field; field.spacing=settings.sample_spacing;
    if(root.operation=="solid.ordered") field.spacing=number(member(root.parameters,"spacing"));
    require(std::isfinite(field.spacing)&&field.spacing>0,"CSG sample spacing must be positive and finite");
    float reference=settings.reference_radius;
    if(root.operation=="solid.ordered") {
        const auto &params=std::get<data::Value::Object>(root.parameters.data);
        if(params.count("referenceRadius")) reference=number(params.at("referenceRadius"));
    }
    require(std::isfinite(reference)&&reference>=0,"CSG reference radius must be nonnegative and finite");
    if(reference>0) field.isovalue=-std::max(0.0f,(field.spacing/reference-1)*reference*0.3f);
    require(std::isfinite(field.isovalue),"CSG coarse offset overflow");
    bool first=true;
    double padding=0;
    auto admit=[&](uint64_t bytes) {
        if(bytes>UINT64_MAX-field.storage) reject(data::ErrorCode::resource_limit,"CSG program size overflow");
        field.storage+=bytes;
        if(usage.vertices>(UINT64_MAX-field.storage)/256) reject(data::ErrorCode::resource_limit,"CSG storage overflow");
        usage.working_bytes=std::max(usage.working_bytes,field.storage+usage.vertices*256);
        checked(monitor.checkpoint(usage));
    };
    std::function<size_t(const data::RecipeNode &,data::Mat4,uint32_t,bool)> lower;
    lower=[&](const data::RecipeNode &n,data::Mat4 parent,uint32_t depth,bool is_root)->size_t {
        // A finite explicit stack guard also protects callers with very large budgets.
        if(depth>128) reject(data::ErrorCode::resource_limit,"CSG lowering depth exceeds 128",n.id.value);
        usage.dependency_depth=std::max(usage.dependency_depth,depth);
        tick(monitor,usage); checked(monitor.checkpoint(usage));
        auto transform=is_root?parent:mm::multiply(parent,n.transform);
        checked(data::validate_transform(n.transform,true));
        checked(data::validate_transform(transform,true));
        if(n.operation=="placement.affine") return lower(*nodes.at(n.inputs[0].reference.value),transform,depth+1,false);
        admit(sizeof(FieldInstruction)*2+64);
        FieldInstruction instruction;
        if(n.operation=="solid.csg" || n.operation=="solid.ordered") {
            instruction.grouped=n.operation=="solid.ordered";
            require(n.inputs.size()>=(instruction.grouped?1u:2u),"CSG has too few operands");
            if(n.inputs.size()>INT_MAX) reject(data::ErrorCode::resource_limit,"CSG operand limit exceeded",n.id.value);
            admit(uint64_t(n.inputs.size())*(sizeof(size_t)+sizeof(int)));
            instruction.inputs.reserve(n.inputs.size()); instruction.operations.reserve(n.inputs.size());
            if(instruction.grouped) {
                keys(n,{"operations","spacing","smoothing","referenceRadius"});
                float spacing=number(member(n.parameters,"spacing"));
                require(spacing>0,"ordered CSG spacing must be positive");
                // Only the output lattice has a spacing. Nested differing requests
                // must not be accepted and then silently ignored.
                require(spacing==field.spacing,"nested ordered CSG spacing differs from output spacing");
                const auto &params=std::get<data::Value::Object>(n.parameters.data);
                if(params.count("referenceRadius")) require(number(params.at("referenceRadius"))==reference,
                    "nested ordered CSG reference radius differs from output reference radius");
                instruction.smoothing=number(member(n.parameters,"smoothing"));
                require(instruction.smoothing>=0,"CSG smoothing must be nonnegative");
                const auto &ops=array(member(n.parameters,"operations"));
                require(ops.size()==n.inputs.size(),"CSG operations must match inputs");
                for(const auto &v:ops) instruction.operations.push_back(operation(v));
                float scale=mm::length(mm::transform_vector(transform,{1,0,0}));
                padding+=double(instruction.smoothing)*std::log(double(n.inputs.size()))*std::max(1.0f,scale);
            } else {
                int op=operation(member(n.parameters,"operator"));
                instruction.operations.assign(n.inputs.size(),op); instruction.operations[0]=0;
            }
            for(const auto &input:n.inputs) {
                require(input.kind==data::InputKind::node&&input.type==data::ProductType::mesh,"CSG requires solid node inputs");
                instruction.inputs.push_back(lower(*nodes.at(input.reference.value),transform,depth+1,false));
            }
        } else {
            instruction.leaf=true;
            auto &p=instruction.primitive;
            data::Mat4 inverse;
            require(mm::inverse(transform,inverse),"CSG transform cannot be inverted");
            p.invTransform=mm::to_c(inverse);
            data::Vec3 lo{},hi{};
            if(n.operation=="primitive.box") {
                p.kind=FAT_PRIM_BOX;
                auto h=mm::scale_vec(vec3(member(n.parameters,"size")),0.5f);
                p.halfExtents=mm::to_c(h); hi=h; lo=mm::scale_vec(h,-1);
            } else if(n.operation=="primitive.sphere") {
                p.kind=FAT_PRIM_SPHERE; p.radius=number(member(n.parameters,"radius"));
                hi={p.radius,p.radius,p.radius}; lo=mm::scale_vec(hi,-1);
                // The production sphere stream measures distance in world units;
                // other fat primitives retain their canonical local distance metric.
                auto center=mm::transform_point(transform,{0,0,0});
                float scale=mm::length(mm::transform_vector(transform,{1,0,0}));
                p.radius*=scale;
                auto translation=mm::identity();
                translation(0,3)=-center.x; translation(1,3)=-center.y; translation(2,3)=-center.z;
                p.invTransform=mm::to_c(translation);
            } else if(n.operation=="primitive.capsule" || n.operation=="primitive.cylinder" ||
                      n.operation=="primitive.cone" || n.operation=="primitive.line") {
                bool capsule=n.operation=="primitive.capsule";
                keys(n,capsule?std::initializer_list<const char *>{"a","b","radius","segments","rings"}:
                    std::initializer_list<const char *>{"a","b","radius0","radius1","segments"});
                auto a=vec3(member(n.parameters,"a")),b=vec3(member(n.parameters,"b"));
                integer(member(n.parameters,"segments"));
                if(capsule) integer(member(n.parameters,"rings"),1);
                float length=mm::length(mm::sub(b,a));
                require(std::isfinite(length)&&(capsule||length>=1e-6f),"invalid CSG segment length");
                p.kind=capsule?FAT_PRIM_CAPSULE:FAT_PRIM_CYLINDER;
                p.segA=mm::to_c(a); p.segB=mm::to_c(b);
                p.r0=number(member(n.parameters,capsule?"radius":"radius0"));
                p.r1=capsule?p.r0:number(member(n.parameters,"radius1"));
                require(p.r0>=0&&p.r1>=0&&p.r0+p.r1>0,"invalid CSG segment radii");
                if(n.operation=="primitive.cylinder") require(p.r0==p.r1,"cylinder requires equal radii");
                float radius=std::max(p.r0,p.r1);
                lo={std::min(a.x,b.x)-radius,std::min(a.y,b.y)-radius,std::min(a.z,b.z)-radius};
                hi={std::max(a.x,b.x)+radius,std::max(a.y,b.y)+radius,std::max(a.z,b.z)+radius};
            } else reject(data::ErrorCode::unsupported_capability,"CSG operand has no analytic solid representation",n.id.value);
            for(int corner=0;corner<8;++corner) {
                auto v=mm::transform_point(transform,{corner&1?hi.x:lo.x,corner&2?hi.y:lo.y,corner&4?hi.z:lo.z});
                require(std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z),"CSG bounds overflow");
                if(first) { field.bounds.min=field.bounds.max=v; first=false; }
                auto &b=field.bounds;
                b.min={std::min(b.min.x,v.x),std::min(b.min.y,v.y),std::min(b.min.z,v.z)};
                b.max={std::max(b.max.x,v.x),std::max(b.max.y,v.y),std::max(b.max.z,v.z)};
            }
        }
        field.program.push_back(std::move(instruction));
        return field.program.size()-1;
    };
    lower(root,{},1,true);
    require(!first&&std::isfinite(padding)&&padding<=std::numeric_limits<float>::max(),"invalid CSG bounds");
    auto expand=data::Vec3{float(padding),float(padding),float(padding)};
    field.bounds.min=mm::sub(field.bounds.min,expand); field.bounds.max=mm::add(field.bounds.max,expand);
    checked(monitor.checkpoint(usage));
    return field;
}

float sample_csg(const CsgField &field,data::Vec3 point,std::vector<float> &values,std::vector<float> &group,
    jobs::JobMonitor &monitor,jobs::ResourceUsage &usage) {
    for(size_t i=0;i<field.program.size();++i) {
        tick(monitor,usage);
        const auto &n=field.program[i];
        if(n.leaf) {
            values[i]=primitive_sdf(&n.primitive,mm::to_c(point));
            if(!std::isfinite(values[i])) reject(data::ErrorCode::backend_failure,"canonical CSG primitive returned a nonfinite distance");
            continue;
        }
        float result=INFINITY;
        for(size_t j=0;j<n.inputs.size();) {
            tick(monitor,usage);
            size_t end=j+1;
            float value=values[n.inputs[j]];
            if(n.grouped) {
                group[0]=value;
                while(end<n.inputs.size()&&n.operations[end]==n.operations[j]) {
                    tick(monitor,usage);
                    group[end-j]=values[n.inputs[end]]; value=std::min(value,group[end-j]); ++end;
                }
                // All-empty groups have no finite distance and must stay empty.
                if(std::isfinite(value)) value=smin_set(group.data(),int(end-j),value,n.smoothing);
            }
            result=fold(result,value,n.operations[j]); j=end;
        }
        if(std::isnan(result)||result==-INFINITY) reject(data::ErrorCode::backend_failure,"nonfinite CSG field");
        values[i]=result;
    }
    return values.back();
}
} // namespace matter::geometry::detail
