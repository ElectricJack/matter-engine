// Preserve OS replace durability, transient-holder retries and real candidate
// open failures at build qualification. Runtime codec fixtures use RAM bytes.
#define MATTER_PART_IO_QUALIFICATION
#include "../tests/part_asset_v2_tests.cpp"
