#include "geometry_properties.h"
#include <cstdio>
#include <cstdlib>
#include <exception>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *bytes,size_t size) {
    try { geometry_properties::fuzz(bytes,size); }
    catch(const std::exception &e) {
        std::fprintf(stderr,"geometry fuzz invariant: %s\n",e.what());
        std::abort(); // libFuzzer retains the reproducing input.
    }
    return 0;
}
