#include <matter/geometry_c.h>
#include "requests.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER(T) {sizeof(T),MATTER_ABI_MAJOR,MATTER_ABI_MINOR}
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static matter_bytes_view bytes(const char *s) { matter_bytes_view v={(const uint8_t *)s,(uint64_t)strlen(s)}; return v; }
static matter_job_envelope job;
static matter_budget budget;
static matter_call_result eval(matter_geometry_api *api,const char *request,const matter_control *control) {
    return api->evaluate(api->instance,bytes(request),&job,&budget,control);
}
static void success(matter_geometry_api *api,matter_call_result r) {
    if(r.status!=MATTER_OK) {
        uint8_t diagnostic[16384]; uint64_t required=0;
        matter_mutable_bytes_view out={diagnostic,sizeof(diagnostic)};
        if(api->diagnostics(api->instance,out,&required)==MATTER_OK) fprintf(stderr,"status %u: %.*s\n",r.status,(int)required,diagnostic);
    }
    CHECK(r.status==MATTER_OK); CHECK(r.result!=0);
}
static void failure(matter_call_result r,matter_status status) { CHECK(r.status==status); CHECK(r.result==0); }
static void print_result(matter_geometry_api *api,matter_call_result r) {
    matter_bytes_view view={0}; success(api,r);
    CHECK(api->read(api->instance,r.result,&view)==MATTER_OK);
    CHECK(fwrite(view.data,1,(size_t)view.byte_length,stdout)==view.byte_length);
    CHECK(api->release(api->instance,r.result)==MATTER_OK);
}
typedef struct callbacks { unsigned cancel,progress; matter_geometry_api *api; } callbacks;
static uint32_t cancelled(void *p) { return ((callbacks *)p)->cancel; }
static void progress(void *p,double fraction,matter_bytes_view stage) {
    callbacks *state=(callbacks *)p;
    CHECK(fraction>=0&&fraction<=1&&stage.byte_length>0);
    ++state->progress; state->cancel=1;
    CHECK(matter_geometry_dispose(state->api)==MATTER_BACKEND_FAILURE);
    failure(eval(state->api,box_request,NULL),MATTER_BACKEND_FAILURE);
}
int main(int argc,char **argv) {
    matter_geometry_api api={.header=HEADER(matter_geometry_api)},other={.header=HEADER(matter_geometry_api)},stale;
    matter_geometry_config config={.header=HEADER(matter_geometry_config)};
    matter_bytes_view view={0}; matter_call_result r,box,sphere;
    uint64_t required=0; uint8_t text[16384]; unsigned i;
    job.header=(matter_abi_header)HEADER(matter_job_envelope);
    job.request_id_utf8=bytes("fixture.request"); job.source_revision_utf8=bytes("revision.1"); job.generation_epoch=UINT64_MAX;
    budget.header=(matter_abi_header)HEADER(matter_budget);
    CHECK(matter_geometry_default_budget(&budget)==MATTER_OK);
    CHECK(matter_geometry_default_config(&config)==MATTER_OK);
    CHECK(matter_geometry_create(NULL,&api)==MATTER_OK);
    CHECK(matter_geometry_create(NULL,&other)==MATTER_OK);
    if(argc==2&&strcmp(argv[1],"--vectors")==0) {
        const char *names[]={"box","extrude","sphere","placement","csg"};
        const char *requests[]={box_request,extrude_request,sphere_request,placement_request,csg_request};
        printf("{");
        for(i=0;i<5;++i) { printf("%s\"%s\":",i?",":"",names[i]); print_result(&api,eval(&api,requests[i],NULL)); }
        box=eval(&api,box_request,NULL); sphere=eval(&api,sphere_request,NULL); success(&api,box); success(&api,sphere);
        printf(",\"query\":"); print_result(&api,api.query(api.instance,box.result,bytes(query_request),&job,&budget,NULL));
        printf(",\"process\":"); print_result(&api,api.process(api.instance,sphere.result,bytes(process_request),&job,&budget,NULL));
        printf("}\n");
        CHECK(matter_geometry_dispose(&api)==MATTER_OK); CHECK(matter_geometry_dispose(&other)==MATTER_OK);
        return 0;
    }
    CHECK(api.capabilities(api.instance,(matter_mutable_bytes_view){NULL,0},&required)==MATTER_RESOURCE_LIMIT);
    CHECK(required>0&&required<sizeof(text));
    CHECK(api.capabilities(api.instance,(matter_mutable_bytes_view){text,required-1},&required)==MATTER_RESOURCE_LIMIT);
    CHECK(api.capabilities(api.instance,(matter_mutable_bytes_view){text,sizeof(text)-1},&required)==MATTER_OK);
    text[required]=0; CHECK(strstr((char *)text,"matter.operation.mesh.smooth")!=NULL);
    CHECK(api.capabilities(api.instance,(matter_mutable_bytes_view){NULL,sizeof(text)},&required)==MATTER_INVALID_INPUT);
    box=eval(&api,box_request,NULL); success(&api,box);
    CHECK(api.read(api.instance,box.result,&view)==MATTER_OK);
    CHECK(view.byte_length>0); /* wire is not NUL terminated */
    { char *copy=(char *)malloc((size_t)view.byte_length+1); CHECK(copy!=NULL);
      memcpy(copy,view.data,(size_t)view.byte_length); copy[view.byte_length]=0;
      CHECK(strstr(copy,"ffffffffffffffff")!=NULL); CHECK(strstr(copy,"fixture.request")!=NULL); free(copy); }
    CHECK(other.read(other.instance,box.result,&view)==MATTER_INVALID_HANDLE); CHECK(view.data==NULL&&view.byte_length==0);
    CHECK(other.release(other.instance,box.result)==MATTER_INVALID_HANDLE);
    failure(other.query(other.instance,box.result,bytes(query_request),&job,&budget,NULL),MATTER_INVALID_HANDLE);
    CHECK(api.read(api.instance,0,&view)==MATTER_INVALID_HANDLE);
    CHECK(api.read(api.instance,box.result,NULL)==MATTER_INVALID_INPUT);
    CHECK(api.release(api.instance,UINT64_MAX)==MATTER_INVALID_HANDLE);
    failure(eval(&api,"{",NULL),MATTER_INVALID_INPUT);
    failure(eval(&api,"{\"binding_version\":2}",NULL),MATTER_UNSUPPORTED_VERSION);
    failure(eval(&api,"{\"binding_version\":1,\"binding_version\":1}",NULL),MATTER_INVALID_INPUT);
    failure(api.evaluate(api.instance,(matter_bytes_view){NULL,1},&job,&budget,NULL),MATTER_INVALID_INPUT);
    failure(api.evaluate(api.instance,(matter_bytes_view){(const uint8_t *)box_request,UINT64_MAX},&job,&budget,NULL),MATTER_INVALID_INPUT);
    failure(api.evaluate(api.instance,(matter_bytes_view){(const uint8_t *)box_request,config.max_request_bytes+1},&job,&budget,NULL),MATTER_RESOURCE_LIMIT);
    { matter_job_envelope bad=job; bad.header.major=2;
      failure(api.evaluate(api.instance,bytes(box_request),&bad,&budget,NULL),MATTER_UNSUPPORTED_VERSION);
      bad=job; bad.header.size=0; failure(api.evaluate(api.instance,bytes(box_request),&bad,&budget,NULL),MATTER_INVALID_INPUT); }
    { matter_budget bad=budget; bad.vertices=1;
      failure(api.evaluate(api.instance,bytes(box_request),&job,&bad,NULL),MATTER_RESOURCE_LIMIT);
      bad=budget; bad.working_bytes=1024; failure(api.evaluate(api.instance,bytes(box_request),&job,&bad,NULL),MATTER_RESOURCE_LIMIT);
      bad=budget; bad.output_bytes=32; failure(api.evaluate(api.instance,bytes(box_request),&job,&bad,NULL),MATTER_RESOURCE_LIMIT); }
    { callbacks state={1,0,&api}; matter_control control={HEADER(matter_control),&state,cancelled,progress};
      failure(eval(&api,box_request,&control),MATTER_CANCELLED); CHECK(state.progress==0);
      state.cancel=0; failure(eval(&api,box_request,&control),MATTER_CANCELLED); CHECK(state.progress>0); }
    { const char unsupported[]="{\"binding_version\":1,\"required_capabilities\":[{\"name\":\"unknown\",\"version\":1}],\"settings\":{}}";
      failure(api.process(api.instance,box.result,bytes(unsupported),&job,&budget,NULL),MATTER_UNSUPPORTED_CAPABILITY); }
    r=api.query(api.instance,box.result,bytes(query_request),&job,&budget,NULL); success(&api,r);
    failure(api.process(api.instance,r.result,bytes(process_request),&job,&budget,NULL),MATTER_INVALID_INPUT);
    CHECK(api.release(api.instance,r.result)==MATTER_OK);
    CHECK(api.release(api.instance,box.result)==MATTER_OK);
    CHECK(api.release(api.instance,box.result)==MATTER_INVALID_HANDLE);
    CHECK(api.read(api.instance,box.result,&view)==MATTER_INVALID_HANDLE);
    for(i=0;i<100;++i) { r=eval(&api,box_request,NULL); success(&api,r); CHECK(r.result!=box.result); CHECK(api.release(api.instance,r.result)==MATTER_OK); }
    { matter_geometry_api limited={.header=HEADER(matter_geometry_api)};
      config.max_results=1; CHECK(matter_geometry_create(&config,&limited)==MATTER_OK);
      r=eval(&limited,box_request,NULL); success(&limited,r); failure(eval(&limited,box_request,NULL),MATTER_RESOURCE_LIMIT);
      CHECK(limited.release(limited.instance,r.result)==MATTER_OK); r=eval(&limited,box_request,NULL); success(&limited,r);
      CHECK(matter_geometry_dispose(&limited)==MATTER_OK);
      config.max_retained_bytes=1; limited.header=(matter_abi_header)HEADER(matter_geometry_api);
      CHECK(matter_geometry_create(&config,&limited)==MATTER_OK); failure(eval(&limited,box_request,NULL),MATTER_RESOURCE_LIMIT);
      CHECK(matter_geometry_dispose(&limited)==MATTER_OK); }
    r=eval(&api,box_request,NULL); success(&api,r); stale=api;
    CHECK(matter_geometry_dispose(&api)==MATTER_OK); CHECK(matter_geometry_dispose(&api)==MATTER_INVALID_HANDLE);
    CHECK(stale.read(stale.instance,r.result,&view)==MATTER_INVALID_HANDLE);
    CHECK(stale.release(stale.instance,r.result)==MATTER_INVALID_HANDLE);
    failure(eval(&stale,box_request,NULL),MATTER_INVALID_HANDLE); CHECK(matter_geometry_dispose(&stale)==MATTER_INVALID_HANDLE);
    CHECK(matter_geometry_dispose(&other)==MATTER_OK);
    printf("BIND-C-OWNERSHIP: %u checks passed\n",checks);
    return 0;
}
