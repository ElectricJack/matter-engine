// bake_trace.cpp — BakeTrace collector implementation (docs/bake-lab.md §II.1).

#include "bake_trace.h"

namespace bake_trace {

namespace {
thread_local Collector* t_current = nullptr;
}  // namespace

Collector::Collector() {
    start_ = std::chrono::steady_clock::now();
    root_.name = kRootName;
    root_.begin_ms = 0.0;
    root_.end_ms = kOpenEndMs;
}

double Collector::now_ms() const {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start_)
        .count();
}

void Collector::begin(const char* name) {
    std::lock_guard<std::mutex> lock(mutex_);
    Span& parent = open_.empty() ? root_ : *open_.back();
    Span s;
    s.name = name;
    s.begin_ms = now_ms();
    s.end_ms = kOpenEndMs;
    parent.children.push_back(std::move(s));
    open_.push_back(&parent.children.back());
}

void Collector::count(const char* name, double v) {
    std::lock_guard<std::mutex> lock(mutex_);
    Span& target = open_.empty() ? root_ : *open_.back();
    target.counters.push_back(Counter{name, v});
}

void Collector::end() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_.empty()) return;  // unbalanced end(): safe no-op
    open_.back()->end_ms = now_ms();
    open_.pop_back();
}

Span Collector::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Span copy = root_;        // deep copy: vectors copy recursively
    copy.end_ms = now_ms();   // root covers the run so far
    return copy;
}

void Collector::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_.clear();
    root_.counters.clear();
    root_.children.clear();
    start_ = std::chrono::steady_clock::now();
}

void set_current(Collector* c) { t_current = c; }

Collector* current() { return t_current; }

}  // namespace bake_trace
