#include <matter/geometry_c.h>
#include "requests.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#ifdef _MSC_VER
#include <malloc.h>
#endif
namespace {
long fail_at=-1;
size_t allocations=0;
void checkpoint() {
    ++allocations;
    if(fail_at==0) { fail_at=-1; throw std::bad_alloc(); }
    if(fail_at>0) --fail_at;
}
void *allocate(size_t bytes) { checkpoint(); if(auto p=std::malloc(bytes?bytes:1)) return p; throw std::bad_alloc(); }
void *aligned(size_t bytes,size_t alignment) {
    checkpoint();
#ifdef _MSC_VER
    if(auto p=_aligned_malloc(bytes?bytes:1,alignment)) return p;
#else
    void *p=nullptr; if(posix_memalign(&p,alignment,bytes?bytes:1)==0) return p;
#endif
    throw std::bad_alloc();
}
void free_aligned(void *p) {
#ifdef _MSC_VER
    _aligned_free(p);
#else
    std::free(p);
#endif
}
}
void *operator new(size_t n) { return allocate(n); }
void *operator new[](size_t n) { return allocate(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p,size_t) noexcept { std::free(p); }
void operator delete[](void *p,size_t) noexcept { std::free(p); }
void *operator new(size_t n,std::align_val_t a) { return aligned(n,size_t(a)); }
void *operator new[](size_t n,std::align_val_t a) { return aligned(n,size_t(a)); }
void operator delete(void *p,std::align_val_t) noexcept { free_aligned(p); }
void operator delete[](void *p,std::align_val_t) noexcept { free_aligned(p); }
void operator delete(void *p,size_t,std::align_val_t) noexcept { free_aligned(p); }
void operator delete[](void *p,size_t,std::align_val_t) noexcept { free_aligned(p); }
#define HEADER(T) {sizeof(T),1,0}
int main() {
    matter_geometry_api api{}; api.header=HEADER(matter_geometry_api);
    matter_budget budget{}; budget.header=HEADER(matter_budget);
    matter_job_envelope job{HEADER(matter_job_envelope),{reinterpret_cast<const uint8_t *>("alloc"),5},
        {reinterpret_cast<const uint8_t *>("revision"),8},1};
    if(matter_geometry_default_budget(&budget)!=MATTER_OK||matter_geometry_create(nullptr,&api)!=MATTER_OK) return 1;
    auto run=[&] { return api.evaluate(api.instance,{reinterpret_cast<const uint8_t *>(box_request),std::strlen(box_request)},&job,&budget,nullptr); };
    auto warm=run(); if(warm.status||api.release(api.instance,warm.result)) return 2;
    allocations=0;
    auto baseline=run(); size_t count=allocations;
    if(baseline.status||api.release(api.instance,baseline.result)) return 3;
    for(size_t i=0;i<count;++i) {
        fail_at=long(i); auto result=run(); bool injected=fail_at<0; fail_at=-1;
        if(!injected||result.status!=MATTER_RESOURCE_LIMIT||result.result) {
            std::fprintf(stderr,"allocation %zu/%zu injected=%d status=%u handle=%llu\n",i,count,int(injected),result.status,
                static_cast<unsigned long long>(result.result));
            uint8_t buffer[2048]; uint64_t n=0;
            if(api.diagnostics(api.instance,{buffer,sizeof(buffer)},&n)==MATTER_OK)
                std::fprintf(stderr,"%.*s\n",int(n),buffer);
            return 4;
        }
    }
    auto recovered=run();
    if(recovered.status||api.release(api.instance,recovered.result)||matter_geometry_dispose(&api)) return 5;
    std::printf("BIND-ALLOCATION: %zu transient failure positions passed; no partial handles, recovery passed\n",count);
    return 0;
}
