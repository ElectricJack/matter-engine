CXX ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -I../include

# Locate the vendored TBB runtime built alongside the static lib. TBB's own
# Makefile emits a versioned dir under thirdparty/tbb/build/. We glob the
# release variant; if you clean it, rebuild with:
#   (cd ../thirdparty/tbb && make tbb)
TBB_LIB_DIR := $(firstword $(wildcard ../thirdparty/tbb/build/linux_*_release))
TBB_LIB_SUBDIR := $(patsubst ../thirdparty/tbb/build/%,%,$(TBB_LIB_DIR))

# Build a full driver command that also stamps an rpath so the smoke_cube
# binary finds libtbb.so.2 at runtime without LD_LIBRARY_PATH gymnastics.
# We use $$ORIGIN (literal, escaped for make) so the rpath is relocatable and
# doesn't embed absolute paths containing spaces (which ld chokes on).
LDFLAGS := -L.. -lautoremesher_core \
           -L$(TBB_LIB_DIR) -ltbb \
           -Wl,-rpath,'$$ORIGIN/../thirdparty/tbb/build/$(TBB_LIB_SUBDIR)' \
           -lpthread -ldl -lrt

smoke_cube: smoke_cube.cpp ../libautoremesher_core.a
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

.PHONY: clean
clean:
	rm -f smoke_cube
