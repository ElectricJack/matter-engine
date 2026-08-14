# platform.mk — shared build configuration, included by every project
# Makefile in this repo (top-level projects via `include ../platform.mk`,
# tests/ subdirectories via `include ../../platform.mk`).
#
# Keep this file's effects narrow and additive: it sets defaults and
# conveniences that every Makefile here wants, not project-specific logic.

# ---- (a) TMP/TEMP -----------------------------------------------------
# MSYS2's make clobbers the Windows TEMP env var, so a build run without a
# working TMP/TEMP fails with "Cannot create temporary file in C:\WINDOWS"
# (see CLAUDE.md's toolchain section). This used to be the caller's job --
# pass TMP=... TEMP=... on every make command line -- derive it here instead.
#
# 2026-08-14: empirically verified from a real MSYS2 UCRT64 shell (PowerShell
# with PATH set to include ucrt64/bin and usr/bin, then plain `make`) that
# $(OS) and $(LOCALAPPDATA) ARE visible to GNU Make 4.4.1 there -- both come
# through as ordinary imported environment variables. BUT: an automated
# harness that invokes make through a different, nested MSYS/Cygwin shell
# layer can lose arbitrary inherited environment variables on the way to
# msys64 binaries -- $(OS) and $(LOCALAPPDATA) included, confirmed with a
# throwaway exported variable that vanished the same way. That is a property
# of that specific invocation path, not of MSYS2 make itself, but it means
# gating purely on $(OS) can silently skip this whole block in exactly the
# automated-build context this file exists to help. So detect Windows two
# ways: the standard $(OS) check, falling back to `uname -s` (a subprocess
# call, not an inherited variable, so it works regardless of the above).
IS_WINDOWS := $(OS)
ifeq ($(IS_WINDOWS),)
IS_WINDOWS := $(if $(filter MINGW% MSYS% CYGWIN%,$(shell uname -s)),Windows_NT,)
endif

ifeq ($(IS_WINDOWS),Windows_NT)
# LOCALAPPDATA is a native Windows path (backslashes); make/gcc want forward
# slashes. Fall back to the known path on this machine if it's ever unset
# (e.g. a stripped-down or nested shell that drops it -- see above).
WIN_LOCALAPPDATA := $(subst \,/,$(LOCALAPPDATA))
ifeq ($(WIN_LOCALAPPDATA),)
WIN_LOCALAPPDATA := C:/Users/webde/AppData/Local
endif
export TMP  := $(WIN_LOCALAPPDATA)/Temp
export TEMP := $(WIN_LOCALAPPDATA)/Temp
endif

# ---- (b) glslc ----------------------------------------------------------
GLSLC ?= glslc

# ---- (c) Parallelism ------------------------------------------------------
# Only the top-level invocation should pick a -j value. Recursive submakes
# (MatterEditor -> `$(MAKE) -C ../MatterEngine3`, MatterEngine3 -> `$(MAKE) -C
# tests`, etc.) inherit MAKEFLAGS -- and the jobserver fds that go with it --
# from their parent automatically; adding a second -j there re-forces the job
# count and prints a "-jN forced in submake" warning instead of just sharing
# the inherited jobserver pool. Also don't override a caller who already
# asked for a specific -j (or -jN via --jobs=N, which normalizes to the same
# "-jN" token in MAKEFLAGS).
ifeq ($(MAKELEVEL),0)
ifeq ($(findstring -j,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(or $(shell nproc 2>/dev/null),4)
endif
endif

# ---- (d) ccache -----------------------------------------------------------
# Detection only for now -- a later phase installs ccache and wires this into
# the compile lines. Empty when ccache isn't on PATH.
CCACHE := $(shell command -v ccache 2>/dev/null)
