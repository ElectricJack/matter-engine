#pragma once

// MatterEditor/src/regen_jobs.h
//
// Bounded, observable job control over the regeneration work the engine
// ALREADY does: WorldSession::reload() and WorldSession::regenerate(seed).
// Neither of those returns anything — they enqueue, supersede whatever was in
// flight, and report through the ordinary BakeStarted / BakePartDone /
// BakeError / BakeFinished event stream. An agent driving the editor had to
// infer completion from a sleep or from log text; this file is the record that
// makes the same lifecycle answerable.
//
// IT ADDS NO SECOND LIFECYCLE. A Job is a LEDGER ENTRY over the real one:
// main.cpp applies the queued job at its post-frame seam (the one place a
// session-heavy operation may run), feeds the engine's own events back in, and
// every state below is a fact observed at one of those two points. Nothing
// here polls the engine, owns a thread, or decides when a bake runs.
//
// WHY IT IS ENGINE-FREE. Not one type from matter/ appears in this header, so
// MatterEditor/tests/test_regen_jobs.cpp can drive overlapping reloads, a
// superseded result, a script failure, shutdown and deterministic seeded
// regeneration with no session, no bake and no window — which is the only way
// those five are testable at all. main.cpp adapts the live sources at the two
// seams; the enums below mirror matter::BakeErrorCode by VALUE and main.cpp
// static_asserts that they still line up.
//
// THE STATES, and why each exists separately:
//
//   accepted    queued here; the seam has not handed it to the engine yet.
//   running     the seam called reload()/regenerate(); the engine owns it.
//   completed   a BakeFinished with zero failed parts landed for it.
//   failed      a BakeFinished reported failed parts, or the editor shut down
//               while the job was still live.
//   cancelled   job.cancel reached it while it was STILL QUEUED, so nothing
//               was ever handed to the engine. This is the only cancellation
//               that is real (see below).
//   superseded  a newer regeneration replaced it. The engine's own contract:
//               "a new request_bake()/reload() supersedes (cancels) an
//               in-flight bake". Distinct from `cancelled` because the caller
//               did not ask for it and something else is now running.
//
// KNOWN LIMIT: A BAKE THAT ABORTS NEVER TERMINATES ITS JOB. A top-level
// install/compose failure inside `execute_bake` emits one BakeError and
// RETURNS — no BakeFinished follows it — so a job whose bake aborts that way
// stays `running` with its diagnostics recorded and reachable through
// job.status, and a bounded job.wait on it expires as `timeout` carrying those
// diagnostics. That is deliberately not reported as `failed`: nothing in the
// event stream distinguishes an aborting BakeError from the per-part
// skip-and-continue errors that DO go on to a BakeFinished, and guessing
// between them would trade a missing terminal state for a wrong one. The fix
// belongs in the engine (a terminal bake-ended event, or a bake generation on
// matter::Event), not in a heuristic here. A per-part script failure — the
// common case — does reach `failed` through BakeFinished's error count.
//
// CANCELLATION IS NOT UNIFORMLY SUPPORTED, AND SAYS SO. WorldSession exposes
// no cancel entry point — supersession is the only mechanism it has. So
// cancelling a job that is still in this queue genuinely works (it never
// reaches the engine), and cancelling a RUNNING one cannot, and answers
// CancelOutcome::UnsupportedRunning rather than pretending or silently
// no-op'ing. That distinction is the whole reason CancelOutcome is not a bool.

#include "agent_protocol.h"
#include "matter/json_doc.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace viewer::jobs {

using Clock = std::chrono::steady_clock;

// How many terminal jobs stay answerable. A job.status for an id older than
// this reports not_found rather than being invented, and job.list says which
// id is the oldest it still holds.
constexpr std::size_t kDefaultHistoryLimit = 64;
// Per-job diagnostic cap. A bake that fails every part would otherwise push a
// result record past the protocol's 1 MiB bound; the overflow is REPORTED
// (`diagnostics_truncated`), never silently dropped.
constexpr std::size_t kMaxDiagnostics = 32;
// Concurrent bounded waits. Each is one pending agent request, and the
// protocol's own pending-request bound is 256.
constexpr std::size_t kMaxWaiters = 64;
constexpr std::size_t kMaxListLimit = 64;
constexpr std::size_t kDefaultListLimit = 16;

enum class Kind { Reload, Regenerate };

enum class State { Accepted, Running, Completed, Failed, Cancelled, Superseded };

// Mirror of matter::BakeErrorCode. Same order, same values — main.cpp
// static_asserts it and casts across, so a new engine code cannot quietly
// arrive here as the wrong name.
enum class ErrorCode { None, Cancelled, OutOfMemory, ScriptError, GpuError, IoError, Internal };

const char* to_string(Kind kind);
const char* to_string(State state);
const char* to_string(ErrorCode code);
bool is_terminal(State state);

// What a cancel request actually did. Deliberately not a bool: "I cancelled
// it", "the backend cannot cancel that", "it had already finished" and "no
// such job" are four different answers and a caller acts differently on each.
enum class CancelOutcome { Cancelled, UnsupportedRunning, AlreadyTerminal, NotFound };

// One engine BakeError, kept with its script location when the message
// carried one. The engine reports script failures as free text with a
// QuickJS stack fragment; `source_*` is that fragment PARSED, and `has_source`
// is false rather than a fabricated file when it had none.
struct Diagnostic {
    std::string module;
    std::string phase;     // "install" | "compose" | "parts" | "gl" | "cone" | ...
    ErrorCode code = ErrorCode::None;
    std::string message;
    bool has_source = false;
    std::string source_file;
    int source_line = 0;
    int source_column = 0;   // 0 when the location named only a line
};

// Pull "<file>.js:<line>[:<column>]" out of an engine error message or a
// QuickJS stack. Takes the FIRST match, which in a stack is the innermost
// frame — the line that actually threw. Returns false and leaves the outputs
// untouched when there is no location to be had; a caller must report the
// message without one rather than guess a file.
bool parse_source_location(const std::string& text, std::string& file, int& line,
                           int& column);

// What was asked for. `world` / `project` are recorded so a job's digest is
// tied to the world it targeted: the same seed against a different world is a
// different regeneration, and a status read after a world switch must not look
// like it describes the current one.
struct StartRequest {
    Kind kind = Kind::Reload;
    bool has_seed = false;
    std::uint64_t seed = 0;
    std::string world;
    std::string project;
};

// One published part-graph root, for the content digest below.
struct RootDigest {
    std::string module;
    std::uint64_t resolved_hash = 0;
};

// FNV-1a over the canonical request text (kind, project, world, seed). Two
// requests digest equal exactly when they asked the engine for the same thing.
std::uint64_t request_digest(const StartRequest& request);

// FNV-1a over the SORTED (module, resolved_hash) roots the bake published.
// Sorted because part-graph iteration order is a hash-map order and is not
// stable between runs, and a digest that changes with iteration order would
// make "the same seed produced the same world" untestable — which is the one
// question this digest exists to answer.
std::uint64_t content_digest(std::vector<RootDigest> roots);

// The ledger entry. Every field is either an input the caller supplied or a
// fact observed at the seam / in the event drain; nothing is inferred.
struct Job {
    std::uint64_t id = 0;
    State state = State::Accepted;
    StartRequest request;
    std::uint64_t digest = 0;

    // Identity when the request was ACCEPTED, so a caller can tell what the
    // job started from as well as what it produced.
    std::uint64_t accepted_scene_revision = 0;
    std::uint64_t accepted_scene_generation = 0;

    // Identity observed when the job's own BakeFinished was drained. Not
    // available for a job that never finished — reported as unavailable rather
    // than as revision 0, which is a real revision.
    bool has_result_scene = false;
    std::uint64_t result_scene_revision = 0;
    std::uint64_t result_scene_generation = 0;
    bool has_content_digest = false;
    std::uint64_t result_content_digest = 0;

    // Advisory progress, straight from BakePartDone. `total` may grow
    // mid-bake (matter/events.h says so), so a percentage built from these
    // can go backwards.
    int parts_done = 0;
    int parts_total = 0;
    std::string phase;
    std::string module;
    int failed_parts = 0;      // BakeFinished::errors

    std::vector<Diagnostic> diagnostics;
    bool diagnostics_truncated = false;

    std::uint64_t superseded_by = 0;   // 0 when superseded by a non-job restart
    std::string terminal_reason;       // always set on a non-completed terminal

    Clock::time_point accepted_at{};
    bool has_started = false;
    Clock::time_point started_at{};
    // Set by the first BakeStarted observed after this job began. Progress and
    // completion are ignored until it is true -- see the ATTRIBUTION note on
    // Registry below.
    bool engine_started = false;
    bool has_finished = false;
    Clock::time_point finished_at{};
};

// The bounded ledger. App thread only — it is touched from the FIFO dispatch
// bridge's app-lane continuations, the post-frame seam and the event drain,
// which are all the same thread.
//
// At most one job is RUNNING. Starting the next one supersedes it, exactly as
// the engine supersedes an in-flight bake, and the supersession is recorded
// HERE at the seam rather than inferred from a later event — the engine's
// cancellation BakeError names no job, and guessing which job it belonged to
// is precisely the false-success this class exists to prevent.
//
// ATTRIBUTION, and the one race it has to survive. `matter::Event` carries no
// bake identity, so "which job does this event belong to" is answered by
// position in the stream. The editor drains events BEFORE the post-frame seam,
// so within a frame the ordering is exact — but a worker thread can queue an
// event in the gap between that drain and the seam, and it would then be read
// on the next frame as if it belonged to the job the seam just started. Two
// rules close that:
//
//   1. a job ignores BakePartDone and BakeFinished until it has seen a
//      BakeStarted OF ITS OWN (`Job::engine_started`). `execute_bake` emits
//      BakeStarted first, always, so a predecessor's completion that was
//      queued in the gap arrives before the successor's BakeStarted and is
//      dropped instead of completing the wrong job;
//   2. a BakeError classified `Cancelled` is never recorded against the
//      running job. Cancellation is what a SUPERSEDED bake reports on its way
//      out, and this ledger has already recorded that supersession as a
//      decision at the seam — so the cancellation belongs to the predecessor
//      by construction, and filing it here would attach a predecessor's death
//      notice to a job that is running perfectly well.
//
// What remains is a bounded reporting gap, not a wrong answer: an event queued
// in that gap is DROPPED rather than misattributed. Closing it completely
// needs a bake generation on the engine's own events, which is an engine
// change, not an editor one.
class Registry {
public:
    explicit Registry(std::size_t history_limit = kDefaultHistoryLimit);

    // Records a new job in `accepted` and returns its id. Ids start at 1,
    // increase monotonically and are never reused.
    std::uint64_t accept(StartRequest request, std::uint64_t scene_revision,
                         std::uint64_t scene_generation, Clock::time_point now);

    bool has_queued() const { return !queue_.empty(); }
    std::uint64_t running_id() const { return running_; }

    // Pops the next queued job, marks it `running`, and supersedes whatever
    // was running. Returns false when the queue is empty; `started` is a COPY
    // of the job as it now stands, which is what the caller needs to decide
    // between reload() and regenerate(seed).
    bool begin_next(Clock::time_point now, Job& started);

    // A reload / world switch that did NOT come from this queue (the toolbar
    // button, the `reload` FIFO verb, viewer.switch_world) still supersedes a
    // running job. Called at the same seam so the ledger never reports a job
    // as running while a different world is baking.
    void note_external_restart(const std::string& reason, Clock::time_point now);

    // --- engine event ingestion, all attributed to the running job ----------
    void on_bake_started(Clock::time_point now);
    void on_part_done(const std::string& module, const std::string& phase, int done,
                      int total);
    void on_bake_error(const std::string& module, const std::string& phase,
                       ErrorCode code, const std::string& message);
    // The job's terminal event. `errors` is BakeFinished::errors; the scene
    // identity and content digest are the ones observed on this drain.
    void on_bake_finished(int errors, std::uint64_t scene_revision,
                          std::uint64_t scene_generation, bool has_content_digest,
                          std::uint64_t content_digest, Clock::time_point now);

    CancelOutcome cancel(std::uint64_t id, Clock::time_point now);

    // Editor teardown: every non-terminal job fails with `reason`. Called once
    // after the frame loop so a pending wait can never outlive the process
    // with an "still running" answer as its last word.
    void shutdown(const std::string& reason, Clock::time_point now);

    const Job* find(std::uint64_t id) const;
    // Most recent `limit` jobs, oldest first, so paging by id stays a total
    // order. Pointers are invalidated by the next accept().
    std::vector<const Job*> recent(std::size_t limit) const;

    std::size_t retained_count() const { return jobs_.size(); }
    std::uint64_t total_accepted() const { return next_id_ - 1; }
    // 0 when nothing is retained.
    std::uint64_t oldest_retained_id() const;

private:
    Job* mutable_find(std::uint64_t id);
    Job* running_job();
    void finish(Job& job, State state, const std::string& reason,
                Clock::time_point now);
    void trim();

    std::size_t history_limit_;
    std::uint64_t next_id_ = 1;
    std::uint64_t running_ = 0;
    std::deque<Job> jobs_;              // ordered by id, oldest first
    std::deque<std::uint64_t> queue_;   // accepted, not yet begun
};

// --- bounded waits ----------------------------------------------------------

// One pending `job.wait`. The deadline is the request's own; a wait that
// expires reports `timeout` with the job's LAST OBSERVED state, never a
// success — a bake that is still running is not a bake that finished.
struct Waiter {
    std::string request_id;
    std::uint64_t ticket_id = 0;
    std::uint64_t job_id = 0;
    Clock::time_point deadline{};
};

class WaitList {
public:
    // False when kMaxWaiters are already pending.
    bool add(Waiter waiter);

    struct Ready {
        Waiter waiter;
        bool timed_out = false;
    };

    // Removes and returns every waiter whose job has reached a terminal state
    // or whose deadline has passed. A waiter for an id the registry no longer
    // retains is returned as timed out rather than kept forever.
    std::vector<Ready> collect(const Registry& registry, Clock::time_point now);
    // Teardown: hand back everything still pending.
    std::vector<Waiter> drain();

    std::size_t size() const { return waiters_.size(); }

private:
    std::vector<Waiter> waiters_;
};

// --- terminal mapping -------------------------------------------------------

// The protocol status a released bounded wait reports. Only a regeneration
// that actually COMPLETED is `ok`; failed, cancelled and superseded are
// `execution_failure` — the protocol's own table already files "shut down"
// and "superseded" under that code — and the state name in the payload says
// which of the three it was. A wait that expired never reaches here: its
// status is `timeout`, which is what keeps a timeout from reading as success.
agent::Status wait_status(State state);
// One line naming the outcome, for the terminal record's `message`. Empty for
// a completed job, which has nothing to explain.
std::string wait_message(const Job& job);

// --- serialization ----------------------------------------------------------

// The full job record shared by every job.* result. Ids, revisions, digests
// and the seed are decimal / hex STRINGS, never JSON numbers, for the same
// reason the rest of this protocol does it: a double truncates them.
matter::jsondoc::Value job_json(const Job& job, Clock::time_point now);

// `job.start` — the accepted record. Success here means "queued", not "done".
matter::jsondoc::Value start_result_json(const Job& job, Clock::time_point now);
// `job.status` — a read; it succeeds even when the job it describes failed.
matter::jsondoc::Value status_result_json(const Job& job, Clock::time_point now);
// `job.wait` — adds `completed` and `timed_out` so the outcome is readable
// without re-deriving it from the state name.
matter::jsondoc::Value wait_result_json(const Job& job, bool timed_out,
                                        Clock::time_point now);
// `job.cancel` — carries what the cancel DID plus the supported/unsupported
// explanation, so an unsupported cancel is self-describing.
matter::jsondoc::Value cancel_result_json(const Job& job, CancelOutcome outcome,
                                          Clock::time_point now);
matter::jsondoc::Value list_result_json(const Registry& registry, std::size_t limit,
                                        Clock::time_point now);
// The not_found answer: the id that was asked for and the window that is still
// answerable, so a caller can tell "never existed" from "aged out".
matter::jsondoc::Value missing_result_json(const Registry& registry,
                                           std::uint64_t id);

// --- argument parsing -------------------------------------------------------
// The descriptor has already checked JSON types; these apply the ranges, the
// enum spellings and the per-operation required/forbidden rules, so a bad
// operation or a seed on a reload is invalid_input rather than a handler
// failure.

// `world` / `project` are NOT caller arguments — main.cpp fills them from the
// live world — so this only reads `operation` and `seed`.
bool parse_start_arguments(const matter::jsondoc::Value& arguments,
                           StartRequest& out, std::string& error);
bool parse_job_id(const matter::jsondoc::Value& arguments, std::uint64_t& out,
                  std::string& error);
bool parse_list_limit(const matter::jsondoc::Value& arguments, std::size_t& out,
                      std::string& error);

}  // namespace viewer::jobs
