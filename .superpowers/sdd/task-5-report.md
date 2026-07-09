# Task 5 Report: memory.hpp RAII Wrappers

## Implementation Summary

Completed all 6 steps of the brief to create C++ RAII wrappers for MemoryLib's C allocators.

### Files Created/Modified

1. **MemoryLib/include/memory.hpp** (created)
   - Header-only C++ RAII wrappers over mem::Arena and mem::Pool
   - `mem::Arena`: Move-only RAII wrapper around MemArena
     - `explicit Arena(size_t initialCap)` constructor
     - Move constructor/assignment operators
     - `allocArray<T>()` template with compile-time checks for trivial destructibility and 8-byte alignment limit
     - `reset()`, `stats()`, `valid()` accessors
   - `mem::Pool`: Move-only RAII wrapper around MemPool
     - Constructor takes objectSize and objectsPerPage
     - Move constructor/assignment operators
     - `alloc()`, `free()` methods
     - `stats()`, `valid()` accessors

2. **MemoryLib/tests/memory_hpp_tests.cpp** (created)
   - C++14 test binary verifying both Arena and Pool RAII wrappers
   - Tests Arena alloc, stats tracking, reset, and move semantics
   - Tests Pool alloc/free, stats, and move semantics
   - Uses Vec3 test struct with trivial destructibility

3. **MemoryLib/Makefile** (modified)
   - Added TEST_CC_OBJS variable to compile C sources once with TEST_FLAGS
   - New rule `$(OBJ_DIR)/t_%.o` for test compilation
   - Updated `test:` target to:
     - Compile C sources with ASan+UBSan via TEST_FLAGS
     - Build memory_tests binary (C test)
     - Build memory_hpp_tests binary (C++ test with g++ -std=c++14)
     - Run both binaries sequentially

### TDD Evidence

**RED (Step 2 - Failing Test):**
```
g++ -Wall -Wextra -g -std=c++14 -I MemoryLib/include -fsanitize=address,undefined \
  -o MemoryLib/memory_hpp_tests MemoryLib/tests/memory_hpp_tests.cpp \
  MemoryLib/src/mem_pool.c MemoryLib/src/mem_arena.c

MemoryLib/tests/memory_hpp_tests.cpp:1:10: fatal error: ../include/memory.hpp: No such file or directory
    1 | #include "../include/memory.hpp"
compilation terminated.
```
Expected failure: Header doesn't exist.

**GREEN (Step 5 - Passing Tests):**
```bash
make -C MemoryLib clean && make -C MemoryLib test

[after compilation...]

./memory_tests
Running MemPool tests...
[... all pool tests pass ...]
All tests passed!

g++ -Wall -Wextra -g -std=c++14 -I./include -fsanitize=address,undefined \
  -o memory_hpp_tests tests/memory_hpp_tests.cpp obj/t_mem_pool.o ...

./memory_hpp_tests
Testing mem::Arena...
Testing mem::Pool...
All memory.hpp tests passed!
```

Both test binaries pass without sanitizer errors or undefined behavior warnings.

**Main Binary (Step 5 - Verification):**
```bash
make -C MemoryLib
./MemoryLib/memorylib
6/6 tests passed
Exit code: 0
```

### Self-Review Checklist

- [x] All brief steps completed in order (1-6)
- [x] Exact signatures match brief specification
- [x] Move semantics correct:
  - Move constructor sets `o.a_ = nullptr` / `o.p_ = nullptr`
  - Move-assigned objects safely destructible (destroy old resource, take new, null source)
  - Moved-from objects in valid but empty state (`valid()` returns false)
- [x] Template `allocArray<T>` has compile-time checks:
  - `std::is_trivially_destructible<T>` enforced
  - `alignof(T) <= 8` enforced (Arena guarantees 8-byte alignment)
- [x] Test output pristine: No warnings, no ASan/UBSan errors
- [x] Only MemoryLib files touched (3 files: 2 created, 1 modified)
- [x] Extern "C" guards from mem_arena.h and mem_pool.h enable C++ linking
- [x] Makefile compiles C sources once, links into both test binaries

### Concerns

None. All steps complete, tests green, no sanitizer violations, move semantics sound, type safety enforced via static_assert.

## Commit

```
[feature/memory-lib 187fb0e] feat(memory-lib): memory.hpp RAII wrappers (mem::Arena, mem::Pool)
 3 files changed, 120 insertions(+)
 create mode 100644 MemoryLib/include/memory.hpp
 create mode 100644 MemoryLib/tests/memory_hpp_tests.cpp
```

Short SHA: `187fb0e`
Subject: `feat(memory-lib): memory.hpp RAII wrappers (mem::Arena, mem::Pool)`
