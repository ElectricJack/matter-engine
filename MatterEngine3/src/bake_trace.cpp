// bake_trace.cpp — BakeTrace collector implementation (docs/bake-lab.md §II.1).
//
// Deliberately simple machinery: a Span tree owned by the Collector, a stack of
// pointers to the spans currently open, and one mutex taken by every entry
// point. The cost of a span is one Span push_back plus an uncontended lock, so
// wrapping a bake phase is free in practice and wrapping an inner loop is not.
//
// The mutex exists for the reader, not the writers: snapshot() may be called
// from the viewer UI or a test while the bake thread is still appending. Writers
// are single-threaded by contract (bake_trace.h), so writer-vs-writer contention
// should never happen.
//
// `t_current` is the thread-local collector the BAKE_SPAN / BAKE_COUNT macros
// resolve against, and it is NON-owning. Whoever calls set_current() owns the
// Collector and must clear it before that Collector dies, or a later macro site
// on the same thread dereferences a dangling pointer. Being thread-local, it
// also means a bake that fans work out to helper threads traces only the thread
// that was set; on every other thread current() is null and the macros compile
// down to a null check.

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

// `name` is stored by pointer and never copied, so it must outlive the
// Collector -- which is why call sites use the bake_trace_names.h constants
// rather than building strings. The pointer pushed onto open_ points into the
// parent's `children` vector and stays valid because only the innermost open
// span is ever appended to: the vector holding an open span is never itself
// appended to while that span is open.
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

double Collector::end() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_.empty()) return 0.0;  // unbalanced end(): safe no-op
    Span* s = open_.back();
    s->end_ms = now_ms();
    open_.pop_back();
    return s->end_ms - s->begin_ms;
}

// Deep-copies the whole span tree (Span holds vectors of Spans, so the copy
// recurses) while holding the mutex: O(spans) allocations, and the bake thread
// blocks for that long. Meant for UI/test polling at human rates, not for a
// per-frame readout. Spans still open at snapshot time keep end_ms ==
// kOpenEndMs; only the returned root is closed off, at the snapshot instant.
Span Collector::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Span copy = root_;        // deep copy: vectors copy recursively
    copy.end_ms = now_ms();   // root covers the run so far
    return copy;
}

// Drops the tree and re-bases the clock, so timestamps recorded before and after
// a reset are not comparable. Writer-side only, and it does not respect open
// spans: open_ is cleared, so any span open across the reset is abandoned and
// its matching end() (including a Scope destructor's) becomes the unbalanced
// no-op.
void Collector::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_.clear();
    root_.counters.clear();
    root_.children.clear();
    start_ = std::chrono::steady_clock::now();
}

// Non-owning and thread-local: set at the bake entry point, cleared with nullptr
// before the Collector goes out of scope. Calls do not stack -- a caller that
// needs to restore a previously installed collector must save and restore it
// itself.
void set_current(Collector* c) { t_current = c; }

Collector* current() { return t_current; }

}  // namespace bake_trace
