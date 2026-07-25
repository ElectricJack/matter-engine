// Headless unit test for ConsoleLog (ring buffer wrap, severity/text
// filtering, clear, thread safety). Does not exercise draw_console_contents
// (which needs a live ImGui frame) — only the ImGui-free ConsoleLog logic.
#include "../console_panel.h"

#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using viewer::ConsoleLog;
using viewer::LogSeverity;

void test_basic_push_and_size() {
    ConsoleLog log(8);
    assert(log.size() == 0);
    log.push(LogSeverity::Info, "one");
    log.push(LogSeverity::Info, "two");
    assert(log.size() == 2);
    std::printf("  basic_push_and_size: OK\n");
}

void test_ring_wrap() {
    ConsoleLog log(4);
    for (int i = 0; i < 4; ++i) {
        log.push(LogSeverity::Info, "msg" + std::to_string(i));
    }
    assert(log.size() == 4);
    // Push beyond capacity: oldest entries should be evicted.
    log.push(LogSeverity::Info, "msg4");
    log.push(LogSeverity::Info, "msg5");
    assert(log.size() == 4);
    auto snap = log.filtered(true, true, true, nullptr);
    assert(snap.entries.size() == 4);
    // Oldest surviving entry should be msg2 (msg0, msg1 evicted).
    assert(snap.entries.front().message == "msg2");
    assert(snap.entries.back().message == "msg5");
    std::printf("  ring_wrap: OK\n");
}

void test_severity_filter() {
    ConsoleLog log(16);
    log.push(LogSeverity::Info, "info-msg");
    log.push(LogSeverity::Warning, "warn-msg");
    log.push(LogSeverity::Error, "error-msg");

    auto all = log.filtered(true, true, true, nullptr);
    assert(all.entries.size() == 3);

    auto no_info = log.filtered(false, true, true, nullptr);
    assert(no_info.entries.size() == 2);
    for (const auto& e : no_info.entries) assert(e.severity != LogSeverity::Info);

    auto only_error = log.filtered(false, false, true, nullptr);
    assert(only_error.entries.size() == 1);
    assert(only_error.entries[0].message == "error-msg");
    std::printf("  severity_filter: OK\n");
}

void test_text_filter() {
    ConsoleLog log(16);
    log.push(LogSeverity::Info, "bake finished: 4 parts");
    log.push(LogSeverity::Error, "[bake] shader compile failed");
    log.push(LogSeverity::Info, "connected to world Demo");

    auto matches = log.filtered(true, true, true, "bake");
    assert(matches.entries.size() == 2);

    auto none = log.filtered(true, true, true, "nonexistent");
    assert(none.entries.empty());

    auto empty_filter = log.filtered(true, true, true, "");
    assert(empty_filter.entries.size() == 3);
    std::printf("  text_filter: OK\n");
}

void test_clear() {
    ConsoleLog log(8);
    log.push(LogSeverity::Info, "a");
    log.push(LogSeverity::Info, "b");
    assert(log.size() == 2);
    log.clear();
    assert(log.size() == 0);
    auto snap = log.filtered(true, true, true, nullptr);
    assert(snap.entries.empty());
    // Buffer remains usable after clear.
    log.push(LogSeverity::Info, "c");
    assert(log.size() == 1);
    std::printf("  clear: OK\n");
}

void test_thread_safety_smoke() {
    ConsoleLog log(4096);
    constexpr int kThreads = 4;
    constexpr int kPerThread = 200;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&log, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                log.push(LogSeverity::Info,
                         "t" + std::to_string(t) + "-" + std::to_string(i));
            }
        });
    }
    for (auto& th : threads) th.join();
    assert(log.size() == static_cast<uint32_t>(kThreads * kPerThread));
    std::printf("  thread_safety_smoke: OK\n");
}

int main() {
    std::printf("test_console_log:\n");
    test_basic_push_and_size();
    test_ring_wrap();
    test_severity_filter();
    test_text_filter();
    test_clear();
    test_thread_safety_smoke();
    std::printf("All tests passed.\n");
    return 0;
}
