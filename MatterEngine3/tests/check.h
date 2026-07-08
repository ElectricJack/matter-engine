// check.h — shared test harness for MatterEngine3 headless test suites.
// Include this once per test translation unit (before any test function).
// Provides:
//   g_failures  — int counter incremented on each failed check.
//   CHECK(cond, msg) — records failure + prints "FAIL: <msg>" on false.
//   check_summary()  — prints "ALL PASS" or "<n> FAILURE(S)" and returns exit code.
//
// Each test file defines its own main() and its own summary printf; this header
// only supplies the shared machinery so the macro is not copy-pasted 25 times.
#pragma once
#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
} while (0)

// Optional helper: prints a canonical summary line and returns the exit code.
// Not all test files call this (each has its own summary wording); it is
// provided for convenience.
static inline int check_summary() {
    if (g_failures == 0) { printf("ALL PASS\n"); return 0; }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
