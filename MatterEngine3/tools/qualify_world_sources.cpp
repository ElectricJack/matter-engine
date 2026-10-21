// Retain real filesystem/layout and shipped-world regressions during build
// qualification. Runtime CTest uses generated, owned source snapshots.
#define MATTER_WORLD_SOURCE_QUALIFICATION
#include "../tests/world_definition_tests.cpp"
