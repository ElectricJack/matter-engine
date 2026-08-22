#pragma once

// Foundation test suites use assert()/cassert for their checks. CMake's
// optimized configurations define NDEBUG, so clear it before the test source
// includes either assertion header. This header is force-included only for
// registered Matter test executables.
#ifdef NDEBUG
#undef NDEBUG
#endif
