// Preserve actual prepared/geometry-page and animation publication regressions
// at build qualification. Runtime PartStore codecs and policy guards use RAM.
#define MATTER_PARTSTORE_SOURCE_QUALIFICATION
#include "../tests/partstore_tests.cpp"
