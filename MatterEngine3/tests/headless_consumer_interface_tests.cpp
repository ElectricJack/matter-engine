#include "provider/local_provider.h"

#if !defined(MATTER_HAVE_SCRIPT_HOST)
#error matter_engine_headless consumers require MATTER_HAVE_SCRIPT_HOST
#endif

int main() {
    viewer::LocalProvider provider(viewer::LocalProviderConfig{});
    auto host_baker_accessor = &viewer::LocalProvider::host_baker;
    (void)provider;
    (void)host_baker_accessor;
    return 0;
}
