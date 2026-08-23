#pragma once

// The legacy headless Make build intentionally keeps runtime assertions active
// in optimized builds. Force-including this header after MSVC's configuration
// defines preserves that ABI and behavior without adding a conflicting
// /UNDEBUG command-line option.
#ifdef NDEBUG
#undef NDEBUG
#endif
