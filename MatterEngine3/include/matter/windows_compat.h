#pragma once

// Matter still consumes a small set of raylib POD types.  Keep Win32's USER
// and GDI declarations out of translation units that also see raylib: both
// APIs expose global names such as Rectangle, CloseWindow, and ShowCursor.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#include <windows.h>

// rpcndr.h publishes `small` as a compatibility macro.  It is not part of the
// Win32 API used by Matter and breaks ordinary C++ local identifiers.
#ifdef small
#undef small
#endif
#endif
