// Flat wasm32 transport for the same function table; no second evaluator.
// Private to geometry.mjs. Every pointer denotes caller-owned allocated memory.
extern "C" matter_status mgw_create(uint32_t max_results,uint32_t retained,uint32_t request,uint32_t *token) {
    if(!token) return MATTER_INVALID_INPUT;
    *token=0;
    auto config=defaults();
    if(max_results) config.max_results=max_results;
    if(retained) config.max_retained_bytes=retained;
    if(request) config.max_request_bytes=request;
    matter_geometry_api api{}; api.header=header<matter_geometry_api>();
    auto status=matter_geometry_create(&config,&api);
    if(status==MATTER_OK) *token=uint32_t(reinterpret_cast<uintptr_t>(api.instance));
    return status;
}
extern "C" matter_status mgw_dispose(uint32_t token) {
    matter_geometry_api api{}; api.header=header<matter_geometry_api>();
    api.instance=reinterpret_cast<void *>(uintptr_t(token));
    return matter_geometry_dispose(&api);
}
extern "C" void mgw_budget(uint64_t *values) {
    auto b=default_budget();
    uint64_t defaults[]={b.operations,b.vertices,b.indices,b.output_bytes,b.working_bytes,
        b.execution_microseconds,b.queries,b.dependency_depth,b.progress_events};
    if(values) std::memcpy(values,defaults,sizeof(defaults));
}
static uint32_t mgw_cancel(void *value) { return *static_cast<uint32_t *>(value); }
extern "C" matter_status mgw_call(uint32_t token,uint32_t operation,uint64_t source,
        const uint8_t *request,uint32_t length,const uint8_t *id,uint32_t id_length,
        const uint8_t *revision,uint32_t revision_length,uint64_t epoch,
        const uint64_t *limits,uint32_t cancelled,uint64_t *result) {
    if(!result) return MATTER_INVALID_INPUT;
    *result=0;
    if(!limits||operation>2||limits[7]>UINT32_MAX||limits[8]>UINT32_MAX) return MATTER_INVALID_INPUT;
    matter_job_envelope job{header<matter_job_envelope>(),{id,id_length},{revision,revision_length},epoch};
    matter_budget budget{header<matter_budget>(),limits[0],limits[1],limits[2],limits[3],limits[4],limits[5],limits[6],
        uint32_t(limits[7]),uint32_t(limits[8])};
    matter_control control{header<matter_control>(),&cancelled,mgw_cancel,nullptr};
    auto out=call(reinterpret_cast<void *>(uintptr_t(token)),int(operation),source,{request,length},&job,&budget,&control);
    *result=out.result; return out.status;
}
extern "C" matter_status mgw_read(uint32_t token,uint64_t handle,uint32_t *view_out) {
    if(!view_out) return MATTER_INVALID_INPUT;
    view_out[0]=view_out[1]=0;
    matter_bytes_view bytes{};
    auto code=read(reinterpret_cast<void *>(uintptr_t(token)),handle,&bytes);
    if(code==MATTER_OK) {
        view_out[0]=uint32_t(reinterpret_cast<uintptr_t>(bytes.data)); view_out[1]=uint32_t(bytes.byte_length);
    }
    return code;
}
extern "C" matter_status mgw_release(uint32_t token,uint64_t handle) {
    return release(reinterpret_cast<void *>(uintptr_t(token)),handle);
}
extern "C" matter_status mgw_info(uint32_t token,uint32_t diagnostic,uint8_t *out,uint32_t capacity,uint32_t *required) {
    if(!required) return MATTER_INVALID_INPUT;
    uint64_t count=0;
    auto code=(diagnostic?get_diagnostics:get_capabilities)(reinterpret_cast<void *>(uintptr_t(token)),{out,capacity},&count);
    *required=uint32_t(count); return code;
}
