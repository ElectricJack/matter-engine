// Diagnostic interposer only. Does not change LLVM arguments or results.
// Build: cc -shared -fPIC -O2 -Wall -Wextra trace_llvm.c -ldl -o trace_llvm.so
// Add LD_PRELOAD=<absolute trace_llvm.so> to the existing bounded native runner.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static void *symbol(const char *name) {
    void *address = dlsym(RTLD_NEXT, name);
    // Vulkan loads its driver with local visibility. Its LLVM dependency
    // can therefore be absent from the preload object's RTLD_NEXT scope.
    // Resolve from the already loaded library without loading a replacement.
    if (!address) {
        void *llvm = dlopen("libLLVM.so.20.1", RTLD_LAZY | RTLD_NOLOAD);
        if (llvm) {
            address = dlsym(llvm, name);
            dlclose(llvm);
        }
    }
    if (!address) {
        fprintf(stderr, "llvm-trace: missing %s: %s\n", name, dlerror());
        abort();
    }
    return address;
}

static void mark(const char *phase, const char *name, size_t length) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(stderr, "llvm-trace t=%lld.%09ld tid=%ld phase=%s name=%.*s\n",
            (long long)now.tv_sec, now.tv_nsec, syscall(SYS_gettid), phase,
            (int)length, name ? name : "");
    fflush(stderr);
}

void *LLVMRunPasses(void *module, const char *passes,
                    void *target, void *options) {
    typedef void *(*Run)(void *, const char *, void *, void *);
    typedef const char *(*Name)(void *, size_t *);
    Run run = (Run)symbol("LLVMRunPasses");
    Name identify = (Name)symbol("LLVMGetModuleIdentifier");
    size_t length = 0;
    const char *name = identify(module, &length);
    mark("LLVMRunPasses.enter", name, length);
    void *result = run(module, passes, target, options);
    mark("LLVMRunPasses.exit", name, length);
    return result;
}

void *LLVMGetPointerToGlobal(void *engine, void *value) {
    typedef void *(*Get)(void *, void *);
    typedef const char *(*Name)(void *, size_t *);
    Get get = (Get)symbol("LLVMGetPointerToGlobal");
    Name identify = (Name)symbol("LLVMGetValueName2");
    size_t length = 0;
    const char *name = identify(value, &length);
    mark("LLVMGetPointerToGlobal.enter", name, length);
    void *result = get(engine, value);
    mark("LLVMGetPointerToGlobal.exit", name, length);
    return result;
}
