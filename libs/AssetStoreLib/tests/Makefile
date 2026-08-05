# libs/AssetStoreLib/tests
#
# asset_store_tests -- the correctness suite (crash recovery, corruption,
#                      eviction, concurrent readers, determinism).
# store_bench       -- the pack-vs-small-files measurement.
#
# Both compile MemoryLib's mem_arena.c from where it lives (CLAUDE.md's rule);
# the library archive does not carry it.
#
# NOT built with ASan, unlike MemoryLib and ParticleFlowLib. The crash-recovery
# test kills a child process with _exit(3) partway through a write on purpose,
# and the whole suite spawns children whose exit codes ARE the assertions --
# a sanitizer that reports through the exit status would be indistinguishable
# from a test failure. MSYS2 UCRT64's libasan is also not reliably present.
# Adding an ASan variant means teaching the child modes to run clean under it
# first; that has not been done.

CXX = g++
LIB_DIR = ..
MEMLIB_DIR = ../../MemoryLib

CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g \
           -I$(LIB_DIR)/include -I$(MEMLIB_DIR)/include
LDLIBS = $(LIB_DIR)/build/libasset_store.a

# pthread on POSIX; Windows threads come from the runtime.
UNAME_S := $(shell uname -s)
ifeq ($(findstring MINGW,$(UNAME_S)),)
ifeq ($(findstring MSYS,$(UNAME_S)),)
  LDLIBS += -lpthread
endif
endif

OBJ_DIR = build

TESTS = $(OBJ_DIR)/asset_store_tests
BENCH = $(OBJ_DIR)/store_bench

all: $(TESTS) $(BENCH)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/mem_arena.o: $(MEMLIB_DIR)/src/mem_arena.c | $(OBJ_DIR)
	gcc -std=c99 -Wall -Wextra -O2 -g -I$(MEMLIB_DIR)/include -c $< -o $@

$(LIB_DIR)/build/libasset_store.a:
	$(MAKE) -C $(LIB_DIR)

$(TESTS): asset_store_tests.cpp $(OBJ_DIR)/mem_arena.o $(LIB_DIR)/build/libasset_store.a | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -o $@ asset_store_tests.cpp $(OBJ_DIR)/mem_arena.o $(LDLIBS)

$(BENCH): store_bench.cpp $(OBJ_DIR)/mem_arena.o $(LIB_DIR)/build/libasset_store.a | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -o $@ store_bench.cpp $(OBJ_DIR)/mem_arena.o $(LDLIBS)

test: $(TESTS)
	./$(TESTS)

bench: $(BENCH)
	./$(BENCH)

clean:
	rm -rf $(OBJ_DIR)

.PHONY: all test bench clean
