#pragma once

// The legacy autoremesher Makefile intentionally builds optimized code with
// geogram's debug/paranoid topology checks enabled. CMake's RelWithDebInfo
// configuration defines NDEBUG globally, so undo it for this target before
// any geogram header selects its assertion policy.
#ifdef NDEBUG
#undef NDEBUG
#endif
