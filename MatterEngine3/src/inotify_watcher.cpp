#include "inotify_watcher.h"
// Real Linux inotify backend implemented in Task 8. Header defines the class
// guarded by __linux__; this TU is intentionally empty until then so the test
// Makefile target links without a real backend (tests use FakeWatcher).
