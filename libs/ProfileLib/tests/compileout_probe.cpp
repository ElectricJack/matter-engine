// Compile-out proof fixture. Built twice by the Makefile `compileout` target:
// once with -DMATTER_PROFILE_ENABLED=1 and once with =0. The build check asserts
// the enabled object CONTAINS the marker string + a register_zone reference, and
// the disabled object contains NEITHER -- proving the macros emit no code, no
// string literal, and no library reference when compiled out (the dist build).
//
// probe_fn is external and non-inline so the optimizer cannot elide it and,
// when enabled, must keep the zone's string literal and static-init reference.

#include "profile.h"

void probe_fn() {
    PROFILE_SCOPE("MATTER_PROFILE_COMPILEOUT_MARKER");
    volatile int x = 0;
    (void)x;
}
