# ProfileLib -- shared engine profiler (P0 core).
# See docs/superpowers/specs/2026-08-07-engine-profiler-design.md
#
#   make            build build/libprofile.a
#   make test       build + run the P0 unit tests
#   make compileout run the compile-OUT proof (asserts on emitted object code)
#   make check      test + compileout
#
# Windows/MSYS2 UCRT64: pass TMP/TEMP explicitly (MSYS make clobbers them),
# matching the rest of the repo:
#   make -C libs/ProfileLib check TMP="C:/Users/.../Temp" TEMP="C:/Users/.../Temp"

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude
BUILD     = build

.PHONY: all test compileout check clean
all: $(BUILD)/libprofile.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/profile.o: src/profile.cpp include/profile.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -c src/profile.cpp -o $@

$(BUILD)/libprofile.a: $(BUILD)/profile.o
	ar rcs $@ $<

$(BUILD)/profile_tests: tests/profile_tests.cpp $(BUILD)/libprofile.a | $(BUILD)
	$(CXX) $(CXXFLAGS) tests/profile_tests.cpp -o $@ -L$(BUILD) -lprofile

test: $(BUILD)/profile_tests
	./$(BUILD)/profile_tests

# Compile the probe both ways and assert on the object code.
compileout: tests/compileout_probe.cpp include/profile.h | $(BUILD)
	@$(CXX) $(CXXFLAGS) -DMATTER_PROFILE_ENABLED=1 -c tests/compileout_probe.cpp -o $(BUILD)/probe_on.o
	@$(CXX) $(CXXFLAGS) -DMATTER_PROFILE_ENABLED=0 -c tests/compileout_probe.cpp -o $(BUILD)/probe_off.o
	@echo "-- enabled build MUST contain the marker + register_zone --"
	@strings $(BUILD)/probe_on.o | grep -q "MATTER_PROFILE_COMPILEOUT_MARKER" \
	  && echo "  [ok] marker string present (enabled)" \
	  || { echo "  [FAIL] marker string missing when enabled"; exit 1; }
	@nm $(BUILD)/probe_on.o | grep -q "register_zone" \
	  && echo "  [ok] register_zone referenced (enabled)" \
	  || { echo "  [FAIL] register_zone not referenced when enabled"; exit 1; }
	@echo "-- disabled build MUST contain NEITHER (compiled out) --"
	@if strings $(BUILD)/probe_off.o | grep -q "MATTER_PROFILE_COMPILEOUT_MARKER"; then \
	  echo "  [FAIL] marker string leaked into compiled-out build"; exit 1; \
	  else echo "  [ok] marker string absent (disabled)"; fi
	@if nm $(BUILD)/probe_off.o | grep -q "register_zone"; then \
	  echo "  [FAIL] register_zone referenced in compiled-out build"; exit 1; \
	  else echo "  [ok] no register_zone reference (disabled)"; fi
	@echo "compile-out proof PASSED"

check: test compileout

clean:
	rm -rf $(BUILD)
