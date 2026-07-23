#include "animation/ozz_adapter.h"

int main() {
    matter::animation::OzzSkeleton skeleton;
    matter::animation::Diagnostics diagnostics;
    // This enters the runtime archive path and must link without any Ozz
    // offline archive.  Invalid data deliberately keeps the test headless.
    return matter::animation::deserialize_skeleton(nullptr, 0, skeleton, diagnostics) ? 1 : 0;
}
