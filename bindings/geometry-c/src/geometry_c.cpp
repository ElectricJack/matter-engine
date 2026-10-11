#include "wire.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#endif

namespace matter::binding {
namespace {
template<class T> matter_abi_header header() { return {uint32_t(sizeof(T)),MATTER_ABI_MAJOR,MATTER_ABI_MINOR}; }
template<class T> void version(const T *p) {
    require(p!=nullptr,"missing versioned structure");
    auto r=matter_check_abi(p->header,uint32_t(sizeof(T)));
    if(r!=MATTER_OK) fail(r,"unsupported structure version or size");
}
matter_call_result outcome(matter_status code,matter_result_handle handle=0) { return {header<matter_call_result>(),code,handle}; }
matter_geometry_config defaults() { return {header<matter_geometry_config>(),64,64ull<<20,8ull<<20,0.1f,0}; }
matter_budget default_budget() {
    s::ResourceBudget b;
    return {header<matter_budget>(),b.operations,b.vertices,b.indices,b.output_bytes,b.working_bytes,
        b.execution_microseconds,b.queries,b.dependency_depth,b.progress_events};
}
struct Stored { std::string wire; bool asset; };
struct Instance {
    explicit Instance(matter_geometry_config c,uint32_t owner):config(c),geometry({c.csg_sample_spacing,c.csg_reference_radius}),owner(owner) {}
    matter_geometry_config config;
    geometry::PrimitiveGeometry geometry;
    uint32_t owner,serial=0;
    uint64_t retained=0;
    std::map<matter_result_handle,Stored> results;
    std::atomic<bool> busy{false};
    std::string diagnostic;
    const char *fallback="[]";
    void error(matter_status code,const char *message) noexcept {
        // Even sustained OOM must return a status and readable diagnostic.
        diagnostic.clear();
        fallback=code==MATTER_RESOURCE_LIMIT?
            "[{\"code\":\"resource_limit\",\"message\":\"binding resource limit\",\"severity\":\"error\"}]":
            "[{\"code\":\"backend_failure\",\"message\":\"binding call failed\",\"severity\":\"error\"}]";
        try {
            static const char *names[]={"ok","invalid_input","unsupported_capability","unsupported_version","missing_dependency",
                "cycle","resource_limit","cancelled","stale_result","backend_failure","invalid_handle"};
            diagnostic=json(A{O{{"code",names[code<=MATTER_INVALID_HANDLE?code:matter_status(MATTER_BACKEND_FAILURE)]},
                {"status",code},{"message",message},{"severity","error"}}},{});
        } catch(...) {}
    }
    matter_status errors(const d::ValidationResult &v) noexcept {
        matter_status code=MATTER_BACKEND_FAILURE;
        for(const auto &e:v.diagnostics) if(e.severity==d::Severity::error) { code=status(e.code); break; }
        error(code,"geometry validation failed");
        try { diagnostic=json(diagnostics(v.diagnostics),{}); } catch(...) {}
        return code;
    }
};
std::mutex registry_mutex;
std::map<uintptr_t,std::shared_ptr<Instance>> registry;
uint32_t next_owner=0;
struct Access {
    std::shared_ptr<Instance> instance;
    explicit Access(void *token) {
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto i=registry.find(reinterpret_cast<uintptr_t>(token));
        if(i==registry.end()) fail(MATTER_INVALID_HANDLE,"disposed or unknown instance");
        if(i->second->busy.exchange(true)) fail(MATTER_BACKEND_FAILURE,"concurrent call or re-entry");
        instance=i->second;
    }
    ~Access() { instance->busy.store(false); }
    Access(const Access &)=delete;
};
template<class Fn> matter_status guarded(void *token,Fn &&fn) noexcept {
    try {
        Access access(token); auto &p=*access.instance;
        try { return fn(p); }
        catch(const d::ValidationResult &v) { return p.errors(v); }
        catch(const Failure &e) { p.error(e.status,e.message); return e.status; }
        catch(const std::bad_alloc &) { p.error(MATTER_RESOURCE_LIMIT,"allocation failed"); return MATTER_RESOURCE_LIMIT; }
        catch(const std::length_error &) { p.error(MATTER_RESOURCE_LIMIT,"length overflow"); return MATTER_RESOURCE_LIMIT; }
        catch(...) { p.error(MATTER_BACKEND_FAILURE,"exception at binding boundary"); return MATTER_BACKEND_FAILURE; }
    } catch(const Failure &e) { return e.status; }
    catch(...) { return MATTER_BACKEND_FAILURE; }
}
std::string_view view(matter_bytes_view bytes,uint64_t limit) {
    require(bytes.byte_length<=uint64_t(PTRDIFF_MAX),"input length is not addressable");
    require(bytes.data!=nullptr||bytes.byte_length==0,"null input data");
#ifdef __EMSCRIPTEN__
    const auto offset=reinterpret_cast<uintptr_t>(bytes.data);
    const auto heap=emscripten_get_heap_size();
    require(offset<=heap&&bytes.byte_length<=heap-offset,"input outside WASM memory");
#endif
    if(bytes.byte_length>limit) fail(MATTER_RESOURCE_LIMIT,"input length exceeds limit");
    return {bytes.byte_length?reinterpret_cast<const char *>(bytes.data):"",size_t(bytes.byte_length)};
}
void mutable_view(matter_mutable_bytes_view bytes) { (void)view({bytes.data,bytes.byte_length},uint64_t(PTRDIFF_MAX)); }
Stored &find(Instance &p,matter_result_handle handle) {
    auto it=p.results.find(handle);
    if(!handle||it==p.results.end()) fail(MATTER_INVALID_HANDLE,"unknown, released or foreign result handle");
    return it->second;
}
matter_status copy(std::string_view text,matter_mutable_bytes_view output,uint64_t *required) {
    require(required!=nullptr,"missing required_bytes output"); *required=text.size();
    mutable_view(output);
    if(output.byte_length<text.size()) return MATTER_RESOURCE_LIMIT;
    if(!text.empty()) std::memcpy(output.data,text.data(),text.size());
    return MATTER_OK;
}
struct Invocation {
    s::JobEnvelope job;
    s::EvaluationOptions options;
    s::JobControl control;
    s::CancellationSource cancellation;
    const matter_control *callbacks;
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    uint64_t input_work=0;
    Invocation(Instance &p,const matter_job_envelope *j,const matter_budget *b,const matter_control *c):callbacks(c) {
        version(j); version(b); if(c) version(c);
        job={std::string(view(j->request_id_utf8,65536)),std::string(view(j->source_revision_utf8,65536)),j->generation_epoch};
        checked(s::validate(job));
        options.budget={b->operations,b->vertices,b->indices,b->output_bytes,b->working_bytes,b->execution_microseconds,
            b->queries,b->dependency_depth,b->progress_events};
        checked(s::validate(options.budget));
        options.validation.supported_capabilities=p.geometry.capabilities();
        auto &l=options.validation.limits;
        l.max_document_bytes=std::min(b->output_bytes,b->working_bytes);
        l.max_resource_bytes=b->working_bytes;
        l.max_vertices=b->vertices; l.max_indices=b->indices;
        l.max_depth=std::min(l.max_depth,b->dependency_depth);
        l.max_nodes=uint32_t(std::min<uint64_t>(l.max_nodes,b->operations));
        l.max_value_nodes=std::min(l.max_value_nodes,b->operations);
        control.cancellation=cancellation.token();
        poll();
        control.progress=[this](const s::Progress &progress) {
            if(callbacks&&callbacks->cancelled&&callbacks->cancelled(callbacks->user)) cancellation.cancel();
            if(callbacks&&callbacks->progress)
                callbacks->progress(callbacks->user,progress.fraction,
                    {reinterpret_cast<const uint8_t *>(progress.stage.data()),progress.stage.size()});
            if(callbacks&&callbacks->cancelled&&callbacks->cancelled(callbacks->user)) cancellation.cancel();
        };
    }
    void poll() {
        if(callbacks&&callbacks->cancelled&&callbacks->cancelled(callbacks->user)) cancellation.cancel();
        if(cancellation.token()->load()) fail(MATTER_CANCELLED,"cancelled at binding checkpoint");
        auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
        if(uint64_t(elapsed)>options.budget.execution_microseconds) fail(MATTER_RESOURCE_LIMIT,"binding execution budget exceeded");
    }
    void admit(uint64_t bytes) {
        if(bytes>(UINT64_MAX-65536)/64) fail(MATTER_RESOURCE_LIMIT,"wire working size overflow");
        uint64_t charge=bytes*64+65536;
        if(charge>options.budget.working_bytes) fail(MATTER_RESOURCE_LIMIT,"wire working budget exceeded");
        options.budget.working_bytes-=charge; input_work+=charge;
    }
    V parse(std::string_view text) {
        V v; auto limits=options.validation.limits;
        // Input/output limits are independent. Admission already bounded input.
        limits.max_document_bytes=text.size();
        checked(d::parse_json(text,v,limits)); return v;
    }
};
matter_call_result call(void *token,int operation,matter_result_handle source,matter_bytes_view bytes,
        const matter_job_envelope *job,const matter_budget *budget,const matter_control *control) noexcept {
    matter_result_handle published=0;
    auto code=guarded(token,[&](Instance &p) {
        p.diagnostic.clear(); p.fallback="[]";
        const Stored *input=nullptr;
        if(operation) { input=&find(p,source); require(input->asset,"query/process requires an asset result"); }
        if(p.results.size()>=p.config.max_results||p.serial==UINT32_MAX)
            fail(MATTER_RESOURCE_LIMIT,"live result or handle sequence limit");
        Invocation invoke(p,job,budget,control);
        auto text=view(bytes,p.config.max_request_bytes);
        invoke.admit(text.size());
        V request=invoke.parse(text);
        auto ver=u32(field(request,"binding_version"));
        if(ver!=1) fail(MATTER_UNSUPPORTED_VERSION,"unsupported binding wire version");
        if(object(request).count("algorithms")) invoke.options.algorithms=capabilities(field(request,"algorithms"));
        V payload;
        s::AssetResult asset=s::AssetResult::failure(invoke.job,d::ErrorCode::backend_failure,"no operation");
        std::optional<s::AssetProduct> source_asset;
        if(input) {
            invoke.admit(input->wire.size());
            V wire=invoke.parse(input->wire);
            d::AssetDocument document;
            // The same strict contract decoder is used by native and WASM.
            auto limits=invoke.options.validation; limits.limits.max_document_bytes=input->wire.size();
            checked(d::decode_asset(json(field(wire,"asset"),limits.limits),document,limits));
            auto resolved=resources(field(wire,"resources"),document.resources,limits.limits.max_resource_bytes);
            source_asset.emplace(std::move(document),std::move(resolved));
        }
        if(operation==0) {
            keys(request,{"binding_version","recipe","resources","algorithms"});
            d::RecipeDocument recipe;
            auto limits=invoke.options.validation;
            // Canonical decimal spellings can be longer than JSON.stringify's
            // compact input. Bound the intermediate copy by admitted wire work.
            limits.limits.max_document_bytes=invoke.input_work;
            checked(d::decode_recipe(json(field(request,"recipe"),limits.limits),recipe,limits));
            s::ResolvedInputs resolved;
            if(object(request).count("resources")) resolved=resources(field(request,"resources"),recipe.resources,limits.limits.max_resource_bytes);
            asset=p.geometry.evaluate(invoke.job,recipe,resolved,invoke.options,invoke.control);
        } else if(operation==1) {
            keys(request,{"binding_version","samples","algorithms"});
            if(array(field(request,"samples")).size()>invoke.options.budget.queries)
                fail(MATTER_RESOURCE_LIMIT,"query count exceeds limit");
            auto batch=query(field(request,"samples"));
            auto result=p.geometry.query(invoke.job,*source_asset,batch,invoke.options,invoke.control);
            if(!result.ok()) throw d::ValidationResult{result.diagnostics()};
            if(result.value()->samples.size()>invoke.options.budget.working_bytes/4096)
                fail(MATTER_RESOURCE_LIMIT,"query serialization budget exceeded");
            payload=O{{"samples",samples(*result.value())}};
        } else {
            keys(request,{"binding_version","required_capabilities","settings","algorithms"});
            s::ProcessingRequest processing{capabilities(field(request,"required_capabilities")),field(request,"settings")};
            asset=p.geometry.process(invoke.job,*source_asset,processing,invoke.options,invoke.control);
        }
        if(operation!=1) {
            if(!asset.ok()) throw d::ValidationResult{asset.diagnostics()};
            if(serialization_work(*asset.value())>invoke.options.budget.working_bytes)
                fail(MATTER_RESOURCE_LIMIT,"asset serialization budget exceeded");
            payload=O{{"asset",d::to_value(asset.value()->document)},{"resources",resources(asset.value()->resources)}};
        }
        auto &out=std::get<O>(payload.data);
        out.emplace("binding_version",1); out.emplace("kind",operation==1?"query":"asset"); out.emplace("job",envelope(invoke.job));
        auto wire=json(payload,invoke.options.validation.limits);
        uint64_t charge=wire.capacity();
        if(charge>p.config.max_retained_bytes-p.retained) fail(MATTER_RESOURCE_LIMIT,"retained result bytes exceeded");
        invoke.poll();
        auto handle=(uint64_t(p.owner)<<32)|uint64_t(++p.serial);
        p.results.emplace(handle,Stored{std::move(wire),operation!=1});
        p.retained+=charge;
        published=handle;
        return matter_status(MATTER_OK);
    });
    return outcome(code,published);
}
matter_call_result evaluate(void *p,matter_bytes_view v,const matter_job_envelope *j,const matter_budget *b,const matter_control *c) {
    return call(p,0,0,v,j,b,c);
}
matter_call_result query_result(void *p,matter_result_handle h,matter_bytes_view v,const matter_job_envelope *j,const matter_budget *b,const matter_control *c) {
    return call(p,1,h,v,j,b,c);
}
matter_call_result process_result(void *p,matter_result_handle h,matter_bytes_view v,const matter_job_envelope *j,const matter_budget *b,const matter_control *c) {
    return call(p,2,h,v,j,b,c);
}
matter_status read(void *p,matter_result_handle h,matter_bytes_view *out) {
    if(out) *out={nullptr,0};
    return guarded(p,[&](Instance &i) {
        require(out!=nullptr,"missing view output"); const auto &r=find(i,h);
        *out={reinterpret_cast<const uint8_t *>(r.wire.data()),r.wire.size()}; return matter_status(MATTER_OK);
    });
}
matter_status release(void *p,matter_result_handle h) {
    return guarded(p,[&](Instance &i) {
        const auto &r=find(i,h); i.retained-=r.wire.capacity(); i.results.erase(h); return matter_status(MATTER_OK);
    });
}
matter_status get_capabilities(void *p,matter_mutable_bytes_view out,uint64_t *required) {
    return guarded(p,[&](Instance &i) {
        auto caps=i.geometry.capabilities(); caps.push_back({"matter.binding.json",1});
        return copy(json(capabilities(caps),{}),out,required);
    });
}
matter_status get_diagnostics(void *p,matter_mutable_bytes_view out,uint64_t *required) {
    return guarded(p,[&](Instance &i) { return copy(i.diagnostic.empty()?std::string_view(i.fallback):std::string_view(i.diagnostic),out,required); });
}
} // namespace
} // namespace matter::binding

using namespace matter::binding;
extern "C" matter_status matter_geometry_default_config(matter_geometry_config *out) {
    try { version(out); *out=defaults(); return MATTER_OK; }
    catch(const Failure &e) { return e.status; } catch(...) { return MATTER_BACKEND_FAILURE; }
}
extern "C" matter_status matter_geometry_default_budget(matter_budget *out) {
    try { version(out); *out=default_budget(); return MATTER_OK; }
    catch(const Failure &e) { return e.status; } catch(...) { return MATTER_BACKEND_FAILURE; }
}
extern "C" matter_status matter_geometry_create(const matter_geometry_config *config,matter_geometry_api *out) {
    try {
        version(out); if(config) version(config);
        auto c=config?*config:defaults();
        require(c.max_results&&c.max_results<=65536&&c.max_request_bytes&&c.max_request_bytes<=uint64_t(PTRDIFF_MAX)&&
            c.max_retained_bytes&&c.max_retained_bytes<=uint64_t(PTRDIFF_MAX),"invalid instance limits");
        require(std::isfinite(c.csg_sample_spacing)&&c.csg_sample_spacing>0&&std::isfinite(c.csg_reference_radius)&&c.csg_reference_radius>=0,
            "invalid CSG settings");
        std::lock_guard<std::mutex> lock(registry_mutex);
        if(next_owner==UINT32_MAX||registry.size()>=1024) return MATTER_RESOURCE_LIMIT;
        const auto owner=++next_owner;
        auto p=std::make_shared<Instance>(c,owner);
        registry.emplace(uintptr_t(owner),std::move(p));
        *out={header<matter_geometry_api>(),reinterpret_cast<void *>(uintptr_t(owner)),evaluate,query_result,process_result,
            read,release,get_capabilities,get_diagnostics};
        return MATTER_OK;
    } catch(const Failure &e) { return e.status; }
    catch(const std::bad_alloc &) { return MATTER_RESOURCE_LIMIT; }
    catch(...) { return MATTER_BACKEND_FAILURE; }
}
extern "C" matter_status matter_geometry_dispose(matter_geometry_api *api) {
    try {
        version(api);
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto i=registry.find(reinterpret_cast<uintptr_t>(api->instance));
        if(i==registry.end()) return MATTER_INVALID_HANDLE;
        if(i->second->busy.load()) return MATTER_BACKEND_FAILURE;
        registry.erase(i); api->instance=nullptr; return MATTER_OK;
    } catch(const Failure &e) { return e.status; } catch(...) { return MATTER_BACKEND_FAILURE; }
}
#ifdef __EMSCRIPTEN__
#include "../wasm/exports.inc"
#endif
