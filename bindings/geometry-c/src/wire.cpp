#include "wire.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace matter::binding {
matter_status status(d::ErrorCode e) {
    switch(e) {
    case d::ErrorCode::invalid_input: return MATTER_INVALID_INPUT;
    case d::ErrorCode::unsupported_capability: return MATTER_UNSUPPORTED_CAPABILITY;
    case d::ErrorCode::unsupported_version: return MATTER_UNSUPPORTED_VERSION;
    case d::ErrorCode::missing_dependency: return MATTER_MISSING_DEPENDENCY;
    case d::ErrorCode::cycle: return MATTER_CYCLE;
    case d::ErrorCode::resource_limit: return MATTER_RESOURCE_LIMIT;
    case d::ErrorCode::cancelled: return MATTER_CANCELLED;
    case d::ErrorCode::stale_result: return MATTER_STALE_RESULT;
    case d::ErrorCode::backend_failure: return MATTER_BACKEND_FAILURE;
    }
    return MATTER_BACKEND_FAILURE;
}
// Retain detailed contract diagnostics at the entrypoint, including source IDs.
void checked(const d::ValidationResult &r) { if (!r.ok()) throw r; }
const O &object(const V &v) {
    auto p=std::get_if<O>(&v.data); require(p!=nullptr,"expected object"); return *p;
}
const A &array(const V &v) {
    auto p=std::get_if<A>(&v.data); require(p!=nullptr,"expected array"); return *p;
}
const V &field(const V &v,const char *name) {
    const auto &o=object(v); auto i=o.find(name); require(i!=o.end(),"missing field"); return i->second;
}
std::string string(const V &v) {
    auto p=std::get_if<std::string>(&v.data); require(p!=nullptr,"expected string"); return *p;
}
double number(const V &v) {
    auto p=std::get_if<double>(&v.data); require(p&&std::isfinite(*p),"expected finite number"); return *p;
}
uint32_t u32(const V &v) {
    double n=number(v); require(n>=0&&n<=UINT32_MAX&&n==std::floor(n),"expected uint32"); return uint32_t(n);
}
void keys(const V &v,std::initializer_list<const char *> names) {
    for (const auto &item:object(v)) {
        bool known=false;
        for (auto name:names) if (item.first==name) known=true;
        require(known,"unknown binding field");
    }
}
std::vector<d::Capability> capabilities(const V &v) {
    std::vector<d::Capability> result;
    for (const auto &c:array(v)) {
        keys(c,{"name","version"});
        result.push_back({string(field(c,"name")),u32(field(c,"version"))});
    }
    return result;
}
V capabilities(const std::vector<d::Capability> &cs) {
    A result;
    for (const auto &c:cs) result.emplace_back(O{{"name",c.name},{"version",c.version}});
    return result;
}
std::string hex64(uint64_t n) {
    std::string r(16,'0');
    for (int i=15;i>=0;--i) { r[size_t(i)]="0123456789abcdef"[n&15]; n>>=4; }
    return r;
}
V envelope(const s::JobEnvelope &e) {
    return O{{"request_id",e.request_id},{"source_revision",e.source_revision},{"generation_epoch",hex64(e.generation_epoch)}};
}
std::string json(const V &v,const d::ValidationLimits &limits) {
    std::string text; checked(d::canonical_json(v,text,limits)); return text;
}
static uint8_t nibble(char c) {
    if(c>='0'&&c<='9') return uint8_t(c-'0');
    if(c>='a'&&c<='f') return uint8_t(c-'a'+10);
    fail(MATTER_INVALID_INPUT,"bytes_hex must be lowercase hexadecimal");
}
s::ResolvedInputs resources(const V &v,const std::vector<d::ResourceDescription> &descriptions,uint64_t limit) {
    s::ResolvedInputs result; std::set<std::string> seen; uint64_t total=0;
    for (const auto &entry:array(v)) {
        keys(entry,{"id","bytes_hex"});
        auto id=string(field(entry,"id"));
        require(seen.insert(id).second,"duplicate resolved resource");
        auto desc=std::find_if(descriptions.begin(),descriptions.end(),[&](const auto &x){return x.id.value==id;});
        if(desc==descriptions.end()) fail(MATTER_MISSING_DEPENDENCY,"resolved resource has no description");
        const auto &value=field(entry,"bytes_hex");
        auto bytes=std::get_if<std::string>(&value.data); require(bytes!=nullptr,"expected bytes_hex string");
        require(bytes->size()%2==0&&bytes->size()/2==desc->byte_length,"resolved resource byte length mismatch");
        if(desc->byte_length>limit-total) fail(MATTER_RESOURCE_LIMIT,"resolved resource budget exceeded");
        total+=desc->byte_length;
        auto storage=std::make_shared<std::vector<uint8_t>>(bytes->size()/2);
        for(size_t i=0;i<storage->size();++i) (*storage)[i]=uint8_t((nibble((*bytes)[2*i])<<4)|nibble((*bytes)[2*i+1]));
        result.resources.push_back({*desc,std::move(storage)});
    }
    return result;
}
V resources(const s::ResolvedInputs &inputs) {
    if(!inputs.assets.empty()) fail(MATTER_UNSUPPORTED_CAPABILITY,"asset dependency output is unsupported");
    A result;
    for(const auto &entry:inputs.resources) {
        require(bool(entry.bytes),"missing resource bytes");
        if(entry.bytes->size()>SIZE_MAX/2) fail(MATTER_RESOURCE_LIMIT,"resource encoding overflow");
        std::string bytes(entry.bytes->size()*2,'0');
        for(size_t i=0;i<entry.bytes->size();++i) {
            bytes[2*i]="0123456789abcdef"[(*entry.bytes)[i]>>4];
            bytes[2*i+1]="0123456789abcdef"[(*entry.bytes)[i]&15];
        }
        result.emplace_back(O{{"id",entry.description.id.value},{"bytes_hex",std::move(bytes)}});
    }
    return result;
}
static d::Vec3 vector3(const V &v) {
    const auto &a=array(v); require(a.size()==3,"expected 3 components");
    d::Vec3 r;
    float *out[]={&r.x,&r.y,&r.z};
    for(size_t i=0;i<3;++i) {
        double n=number(a[i]); require(std::abs(n)<=std::numeric_limits<float>::max(),"float32 overflow"); *out[i]=float(n);
    }
    return r;
}
static V vector3(d::Vec3 v) { return A{v.x,v.y,v.z}; }
s::QueryBatch query(const V &v) {
    s::QueryBatch batch;
    for(const auto &entry:array(v)) {
        keys(entry,{"prototype","kind","position","direction","max_distance_metres"});
        s::SurfaceQuery q; q.prototype={string(field(entry,"prototype"))};
        auto kind=string(field(entry,"kind"));
        require(kind=="closest_surface"||kind=="ray_surface","unknown query kind");
        q.kind=kind=="ray_surface"?s::QueryKind::ray_surface:s::QueryKind::closest_surface;
        q.position=vector3(field(entry,"position"));
        if(object(entry).count("direction")) q.direction=vector3(field(entry,"direction"));
        if(object(entry).count("max_distance_metres")) {
            double n=number(field(entry,"max_distance_metres"));
            require(n>0&&n<=std::numeric_limits<float>::max(),"invalid query distance");
            q.max_distance_metres=float(n);
        }
        batch.samples.push_back(std::move(q));
    }
    return batch;
}
V samples(const s::QueryProduct &product) {
    A result;
    for(const auto &q:product.samples) result.emplace_back(O{{"hit",q.hit},{"position",vector3(q.position)},
        {"normal",vector3(q.normal)},{"triangle",q.triangle},{"distance_metres",q.distance_metres}});
    return result;
}
V diagnostics(const std::vector<d::Diagnostic> &ds) {
    A result;
    for(const auto &diag:ds) {
        A path; for(const auto &part:diag.dependency_path) path.emplace_back(part);
        V source;
        if(diag.source) source=O{{"uri",diag.source->uri},{"line",diag.source->line},{"column",diag.source->column}};
        result.emplace_back(O{{"code",d::code_name(diag.code)},{"message",diag.message},{"node_id",diag.node_id},
            {"severity",diag.severity==d::Severity::error?"error":diag.severity==d::Severity::warning?"warning":"info"},
            {"source",std::move(source)},{"dependency_path",std::move(path)}});
    }
    return result;
}
uint64_t serialization_work(const s::AssetProduct &product) {
    // Value trees own map nodes, nested vectors and scalar variants. These
    // deliberately conservative charges precede conversion, not just encoding.
    uint64_t units=1024;
    auto add=[&](uint64_t count) {
        if(count>(UINT64_MAX/256)-units) fail(MATTER_RESOURCE_LIMIT,"serialization size overflow");
        units+=count;
    };
    for(const auto &m:product.document.meshes) {
        add(64+m.id.value.size()); add(m.positions.size()*4); add(m.normals.size()*4);
        add(m.tangents.size()*5); add(m.indices.size()); add(m.submeshes.size()*16);
        for(const auto &uv:m.uv_sets) add(uv.size()*3);
    }
    add(product.document.instances.size()*64); add(product.document.materials.size()*128);
    for(const auto &r:product.resources.resources) add(r.description.byte_length);
    return units*256;
}
} // namespace matter::binding
