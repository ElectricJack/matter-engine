CXX ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -I../include

# libautoremesher_core.a is self-contained -- its TBB replacement
# (../thirdparty/tbb_shim/) is header-only -- so the link is just the archive
# plus the platform's thread runtime. No companion shared library to locate,
# and no rpath to stamp.
#
# Ask the compiler what it targets rather than reading $(OS)/$(MSYSTEM): those
# depend on which shell invoked make (Git Bash does not export either), while
# -dumpmachine is always right.
TARGET_TRIPLE := $(shell $(CXX) -dumpmachine)
LDFLAGS := -L.. -lautoremesher_core
ifeq ($(findstring mingw,$(TARGET_TRIPLE)),)
  LDFLAGS += -lpthread -ldl
endif

smoke_cube: smoke_cube.cpp ../libautoremesher_core.a
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

.PHONY: clean
clean:
	rm -f smoke_cube smoke_cube.exe
