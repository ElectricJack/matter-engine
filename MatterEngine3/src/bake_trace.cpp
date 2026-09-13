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

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

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

namespace {
void json_string(std::ostream& out, const char* text) {
    if (!text) { out << "null"; return; }
    out << '"';
    const char* hex = "0123456789abcdef";
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c) {
        if (*c == '"' || *c == '\\') out << '\\' << static_cast<char>(*c);
        else if (*c < 0x20) out << "\\u00" << hex[*c >> 4] << hex[*c & 15];
        else out << static_cast<char>(*c);
    }
    out << '"';
}
void json_number(std::ostream& out, double value) {
    if (std::isfinite(value)) out << value;
    else out << "null";
}
void json_span(std::ostream& out, const Span& span) {
    out << "{\"name\":"; json_string(out, span.name);
    out << ",\"begin_ms\":"; json_number(out, span.begin_ms);
    const bool open = span.end_ms == kOpenEndMs;
    out << ",\"end_ms\":";
    if (open) out << "null"; else json_number(out, span.end_ms);
    out << ",\"duration_ms\":";
    if (open) out << "null"; else json_number(out, span.end_ms - span.begin_ms);
    out << ",\"open\":" << (open ? "true" : "false") << ",\"counters\":[";
    for (size_t i = 0; i < span.counters.size(); ++i) {
        if (i) out << ',';
        out << "{\"name\":"; json_string(out, span.counters[i].name);
        out << ",\"value\":"; json_number(out, span.counters[i].value); out << '}';
    }
    out << "],\"children\":[";
    for (size_t i = 0; i < span.children.size(); ++i) {
        if (i) out << ',';
        json_span(out, span.children[i]);
    }
    out << "]}";
}
} // namespace

std::string snapshot_json(const Span& snapshot) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << "{\"schema_version\":1,\"time_unit\":\"ms\",\"root\":";
    json_span(out, snapshot);
    out << "}\n";
    return out.str();
}

bool write_snapshot_json(const Span& snapshot, const std::string& path,
                         std::string& error) {
    error.clear();
    if (!std::filesystem::path(path).is_absolute()) {
        error = "MATTER_BAKE_TRACE requires an absolute output path: " + path;
        return false;
    }
    // Serialize before opening so serialization failures cannot truncate an
    // existing diagnostic. This export is deliberately not a durable artifact.
    const std::string json = snapshot_json(snapshot);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { error = "cannot open bake trace output: " + path; return false; }
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    out.close();
    if (!out) { error = "cannot write/close bake trace output: " + path; return false; }
    return true;
}

// Non-owning and thread-local: set at the bake entry point, cleared with nullptr
// before the Collector goes out of scope. Calls do not stack -- a caller that
// needs to restore a previously installed collector must save and restore it
// itself.
void set_current(Collector* c) { t_current = c; }

Collector* current() { return t_current; }

}  // namespace bake_trace
