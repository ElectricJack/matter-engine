// Headless cover for the regeneration job ledger (job.start / job.status /
// job.wait / job.cancel / job.list).
//
// The five outcomes the acceptance criteria name -- overlapping reloads, a
// superseded result, a script failure with its file:line, editor shutdown, and
// deterministic seeded regeneration -- are all decidable from plain values,
// because regen_jobs.cpp deliberately holds no session, bake or renderer. Time
// is injected as steady_clock points, so the bounded-wait tests are exact
// rather than sleep-based.

#include "../src/regen_jobs.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using matter::jsondoc::Value;
namespace jobs = viewer::jobs;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__,      \
                         __LINE__);                                            \
            std::exit(1);                                                      \
        }                                                                      \
    } while (false)

const jobs::Clock::time_point kEpoch{};

jobs::Clock::time_point at_ms(int milliseconds) {
    return kEpoch + std::chrono::milliseconds(milliseconds);
}

jobs::StartRequest reload_request() {
    jobs::StartRequest request;
    request.kind = jobs::Kind::Reload;
    request.world = "StreamMountain";
    request.project = "projects/world_demo";
    return request;
}

jobs::StartRequest regenerate_request(std::uint64_t seed) {
    jobs::StartRequest request = reload_request();
    request.kind = jobs::Kind::Regenerate;
    request.has_seed = true;
    request.seed = seed;
    return request;
}

const Value& field(const Value& parent, const char* key, const char* message) {
    const Value* found = parent.find(key);
    CHECK(found != nullptr, message);
    return *found;
}

bool string_field_is(const Value& parent, const char* key, const char* expected) {
    const Value* found = parent.find(key);
    return found && found->kind == Value::Kind::String && found->str == expected;
}

bool is_unavailable(const Value& value) {
    const Value* available = value.find("available");
    const Value* reason = value.find("reason");
    return value.kind == Value::Kind::Object && available &&
           available->kind == Value::Kind::Bool && !available->b && reason &&
           reason->kind == Value::Kind::String && !reason->str.empty();
}

Value string_value(const char* text) {
    Value value;
    value.kind = Value::Kind::String;
    value.str = text;
    return value;
}

Value number_value(double number) {
    Value value;
    value.kind = Value::Kind::Number;
    value.num = number;
    return value;
}

// --- lifecycle --------------------------------------------------------------

void test_accept_is_not_completion() {
    jobs::Registry registry;
    const std::uint64_t id =
        registry.accept(reload_request(), 12, 4, at_ms(0));
    CHECK(id == 1, "job ids start at 1");
    const jobs::Job* job = registry.find(id);
    CHECK(job != nullptr, "an accepted job is immediately findable");
    CHECK(job->state == jobs::State::Accepted, "a new job is accepted, not running");
    CHECK(!jobs::is_terminal(job->state), "accepted is not terminal");
    CHECK(registry.running_id() == 0, "accepting does not start anything");
    CHECK(job->accepted_scene_revision == 12 && job->accepted_scene_generation == 4,
          "the accept-time scene identity is recorded");

    const Value result = jobs::start_result_json(*job, at_ms(1));
    const Value& record = field(result, "job", "start result carries the job");
    CHECK(string_field_is(record, "state", "accepted"),
          "job.start reports accepted, never completed");
    CHECK(string_field_is(record, "job_id", "1"),
          "ids serialize as decimal strings, not JSON numbers");
    const Value& result_block = field(record, "result", "job carries a result block");
    CHECK(is_unavailable(field(result_block, "scene_revision",
                               "result names scene_revision")),
          "a job with no BakeFinished reports no scene revision rather than 0");
}

void test_begin_next_runs_one_job() {
    jobs::Registry registry;
    const std::uint64_t id = registry.accept(regenerate_request(7), 1, 1, at_ms(0));
    jobs::Job started;
    CHECK(registry.begin_next(at_ms(5), started), "the queued job begins");
    CHECK(started.id == id && started.state == jobs::State::Running,
          "begin_next hands back the now-running job");
    CHECK(started.request.has_seed && started.request.seed == 7,
          "the seed survives to the seam that calls regenerate()");
    CHECK(registry.running_id() == id, "the registry tracks one running job");
    jobs::Job ignored;
    CHECK(!registry.begin_next(at_ms(6), ignored), "an empty queue begins nothing");
    CHECK(registry.running_id() == id, "a failed begin_next does not clear running");
}

// --- overlapping reloads and supersession -----------------------------------

void test_overlapping_reloads_supersede() {
    jobs::Registry registry;
    const std::uint64_t first = registry.accept(reload_request(), 1, 1, at_ms(0));
    const std::uint64_t second = registry.accept(reload_request(), 1, 1, at_ms(1));
    jobs::Job started;
    CHECK(registry.begin_next(at_ms(2), started) && started.id == first,
          "the queue is FIFO");
    registry.on_bake_started(at_ms(3));
    registry.on_part_done("Terrain", "parts", 3, 10);

    CHECK(registry.begin_next(at_ms(4), started) && started.id == second,
          "the second reload begins while the first is still in flight");
    const jobs::Job* superseded = registry.find(first);
    CHECK(superseded->state == jobs::State::Superseded,
          "an overlapped reload is superseded, not completed and not cancelled");
    CHECK(superseded->superseded_by == second,
          "the superseding job is named, so the caller can follow it");
    CHECK(superseded->has_finished, "a superseded job is terminal");
    CHECK(registry.running_id() == second, "only the newest job runs");

    // ATTRIBUTION rule 1. The first job's own bake could have finished in the
    // gap between the editor's event drain and the seam that started the
    // second; that completion arrives BEFORE the second job's BakeStarted and
    // must be dropped, not read as the second job succeeding.
    registry.on_bake_finished(0, 44, 9, true, 0xabcdull, at_ms(20));
    CHECK(registry.find(first)->state == jobs::State::Superseded,
          "a later completion does not resurrect the superseded job");
    CHECK(!registry.find(first)->has_result_scene,
          "the superseded job claims no resulting scene revision");
    CHECK(registry.find(second)->state == jobs::State::Running,
          "a completion that predates this job's own BakeStarted is dropped");
    CHECK(!registry.find(second)->has_result_scene,
          "and it claims no scene revision from it");

    // Once the engine confirms THIS job's bake, its own completion lands.
    registry.on_bake_started(at_ms(21));
    registry.on_bake_finished(0, 51, 10, true, 0xbeefull, at_ms(30));
    CHECK(registry.find(second)->state == jobs::State::Completed,
          "the running job takes its own completion");
    CHECK(registry.find(second)->result_scene_revision == 51,
          "the completing job records the scene revision it produced");
}

// ATTRIBUTION rule 2: cancellation is the SUPERSEDED bake's exit notice, and
// supersession is already recorded at the seam, so it must not be filed as a
// diagnostic of the job that is now running.
void test_cancellation_error_is_not_the_running_jobs() {
    jobs::Registry registry;
    registry.accept(reload_request(), 1, 1, at_ms(0));
    const std::uint64_t second = registry.accept(reload_request(), 1, 1, at_ms(1));
    jobs::Job started;
    registry.begin_next(at_ms(2), started);
    registry.on_bake_started(at_ms(3));
    registry.begin_next(at_ms(4), started);   // supersedes the first
    // The engine tears the superseded bake down and reports it cancelled.
    registry.on_bake_error("Terrain", "parts", jobs::ErrorCode::Cancelled,
                           "cancelled between parts");
    registry.on_bake_started(at_ms(5));
    registry.on_bake_finished(0, 12, 4, false, 0, at_ms(6));
    const jobs::Job* job = registry.find(second);
    CHECK(job->state == jobs::State::Completed,
          "the predecessor's cancellation does not fail the running job");
    CHECK(job->diagnostics.empty(),
          "nor does it become the running job's diagnostic");
}

void test_external_restart_supersedes() {
    jobs::Registry registry;
    const std::uint64_t id = registry.accept(reload_request(), 1, 1, at_ms(0));
    jobs::Job started;
    CHECK(registry.begin_next(at_ms(1), started), "the job starts");
    registry.note_external_restart("the toolbar Reload button restarted the world",
                                   at_ms(2));
    const jobs::Job* job = registry.find(id);
    CHECK(job->state == jobs::State::Superseded,
          "a UI reload supersedes a running job rather than leaving it 'running'");
    CHECK(job->superseded_by == 0,
          "a non-job restart names no superseding job id");
    CHECK(job->terminal_reason.find("toolbar") != std::string::npos,
          "the reason says what restarted the world");
    CHECK(registry.running_id() == 0, "nothing is running after an external restart");

    // Events after the external restart belong to no job at all.
    registry.on_bake_finished(0, 99, 3, false, 0, at_ms(30));
    CHECK(registry.find(id)->state == jobs::State::Superseded &&
              !registry.find(id)->has_result_scene,
          "an orphan completion is dropped, not attributed to the last job");
}

// --- script failure ---------------------------------------------------------

void test_script_failure_preserves_file_and_line() {
    jobs::Registry registry;
    const std::uint64_t id = registry.accept(reload_request(), 5, 2, at_ms(0));
    jobs::Job started;
    registry.begin_next(at_ms(1), started);
    registry.on_bake_started(at_ms(2));
    registry.on_bake_error(
        "Rock", "install", jobs::ErrorCode::ScriptError,
        "TypeError: r.build is not a function\n    at build "
        "(objects/Rock.js:88:12)\n    at <eval> (objects/World.js:12:3)");
    registry.on_bake_finished(1, 21, 6, true, 0x99ull, at_ms(40));

    const jobs::Job* job = registry.find(id);
    CHECK(job->state == jobs::State::Failed,
          "a bake that reports failed parts is failed, not completed");
    CHECK(job->failed_parts == 1, "the failed-part count is kept");
    CHECK(job->diagnostics.size() == 1, "the BakeError is retained");
    const jobs::Diagnostic& diagnostic = job->diagnostics[0];
    CHECK(diagnostic.has_source, "a QuickJS stack yields a source location");
    CHECK(diagnostic.source_file == "objects/Rock.js",
          "the innermost frame's file survives");
    CHECK(diagnostic.source_line == 88 && diagnostic.source_column == 12,
          "line and column survive");
    CHECK(diagnostic.code == jobs::ErrorCode::ScriptError,
          "the engine's classification survives");
    CHECK(diagnostic.message.find("TypeError") != std::string::npos,
          "the original message is not replaced by the parsed location");

    const Value record = jobs::job_json(*job, at_ms(41));
    const Value& diagnostics =
        field(record, "diagnostics", "the record carries diagnostics");
    CHECK(diagnostics.kind == Value::Kind::Array && diagnostics.arr.size() == 1,
          "one diagnostic serializes");
    const Value& source =
        field(diagnostics.arr[0], "source", "a diagnostic carries source");
    CHECK(string_field_is(source, "file", "objects/Rock.js"),
          "the serialized diagnostic keeps the file");
    CHECK(string_field_is(diagnostics.arr[0], "code", "script_error"),
          "the serialized diagnostic keeps the code spelling");
}

void test_source_location_parsing() {
    std::string file;
    int line = 0;
    int column = 0;
    CHECK(jobs::parse_source_location("bake failed for objects/Tree.js:14", file,
                                      line, column),
          "a bare file:line is located");
    CHECK(file == "objects/Tree.js" && line == 14 && column == 0,
          "a missing column stays 0 rather than being invented");

    CHECK(jobs::parse_source_location(
              "SyntaxError\n    at (shared-lib/noise.mjs:3:7)\n    at "
              "(objects/A.js:9:1)",
              file, line, column),
          "a stack is located");
    CHECK(file == "shared-lib/noise.mjs" && line == 3 && column == 7,
          "the innermost frame wins, not the last one");

    file.clear();
    line = 0;
    column = 0;
    CHECK(!jobs::parse_source_location("cannot read child Rock.js in any object root",
                                       file, line, column),
          "a message with no line number is not given one");
    CHECK(file.empty() && line == 0, "a failed parse leaves the outputs alone");
    CHECK(!jobs::parse_source_location("out of memory", file, line, column),
          "an unrelated message has no location");
}

void test_diagnostics_are_bounded() {
    jobs::Registry registry;
    const std::uint64_t id = registry.accept(reload_request(), 1, 1, at_ms(0));
    jobs::Job started;
    registry.begin_next(at_ms(1), started);
    for (std::size_t index = 0; index < jobs::kMaxDiagnostics + 5; ++index)
        registry.on_bake_error("M", "parts", jobs::ErrorCode::IoError, "missing");
    const jobs::Job* job = registry.find(id);
    CHECK(job->diagnostics.size() == jobs::kMaxDiagnostics,
          "diagnostics are capped so a result cannot exceed the protocol bound");
    CHECK(job->diagnostics_truncated,
          "the overflow is reported, never silently dropped");
}

// --- cancellation -----------------------------------------------------------

void test_cancel_queued_job_is_real() {
    jobs::Registry registry;
    const std::uint64_t id = registry.accept(reload_request(), 1, 1, at_ms(0));
    CHECK(registry.cancel(id, at_ms(1)) == jobs::CancelOutcome::Cancelled,
          "a queued job can genuinely be cancelled");
    const jobs::Job* job = registry.find(id);
    CHECK(job->state == jobs::State::Cancelled,
          "the cancelled job is cancelled, not superseded");
    jobs::Job started;
    CHECK(!registry.begin_next(at_ms(2), started),
          "a cancelled job is never handed to the engine");
}

void test_cancel_running_job_is_unsupported() {
    jobs::Registry registry;
    const std::uint64_t id = registry.accept(reload_request(), 1, 1, at_ms(0));
    jobs::Job started;
    registry.begin_next(at_ms(1), started);
    CHECK(registry.cancel(id, at_ms(2)) == jobs::CancelOutcome::UnsupportedRunning,
          "the backend exposes no cancel for a running bake");
    const jobs::Job* job = registry.find(id);
    CHECK(job->state == jobs::State::Running,
          "an unsupported cancel changes nothing; it does not fake a stop");

    const Value result = jobs::cancel_result_json(
        *job, jobs::CancelOutcome::UnsupportedRunning, at_ms(3));
    const Value& cancel = field(result, "cancel", "the result explains the cancel");
    const Value* supported = cancel.find("supported");
    const Value* cancelled = cancel.find("cancelled");
    CHECK(supported && supported->kind == Value::Kind::Bool && !supported->b,
          "unsupported is stated outright");
    CHECK(cancelled && cancelled->kind == Value::Kind::Bool && !cancelled->b,
          "nothing was cancelled");
    CHECK(cancel.find("alternative") != nullptr,
          "the supported alternative (supersession) is named");
}

void test_cancel_terminal_and_missing() {
    jobs::Registry registry;
    const std::uint64_t id = registry.accept(reload_request(), 1, 1, at_ms(0));
    jobs::Job started;
    registry.begin_next(at_ms(1), started);
    registry.on_bake_started(at_ms(1));
    registry.on_bake_finished(0, 3, 1, false, 0, at_ms(2));
    CHECK(registry.cancel(id, at_ms(3)) == jobs::CancelOutcome::AlreadyTerminal,
          "cancelling a finished job is not an error and changes nothing");
    CHECK(registry.find(id)->state == jobs::State::Completed,
          "the completed job stays completed");
    CHECK(registry.cancel(9999, at_ms(3)) == jobs::CancelOutcome::NotFound,
          "an unknown id is not_found, not a silent success");
}

// --- bounded wait -----------------------------------------------------------

void test_wait_releases_on_terminal_state() {
    jobs::Registry registry;
    jobs::WaitList waits;
    const std::uint64_t id = registry.accept(reload_request(), 1, 1, at_ms(0));
    jobs::Waiter waiter;
    waiter.request_id = "req-1";
    waiter.ticket_id = 27;
    waiter.job_id = id;
    waiter.deadline = at_ms(5000);
    CHECK(waits.add(waiter), "a wait is accepted");
    CHECK(waits.collect(registry, at_ms(10)).empty(),
          "a queued job releases no waiter");

    jobs::Job started;
    registry.begin_next(at_ms(11), started);
    registry.on_bake_started(at_ms(11));
    CHECK(waits.collect(registry, at_ms(12)).empty(),
          "a RUNNING job releases no waiter either");
    registry.on_bake_finished(0, 7, 2, true, 0x1234ull, at_ms(20));
    const std::vector<jobs::WaitList::Ready> ready =
        waits.collect(registry, at_ms(21));
    CHECK(ready.size() == 1, "the terminal state releases the waiter");
    CHECK(!ready[0].timed_out, "a released-by-completion waiter did not time out");
    CHECK(ready[0].waiter.request_id == "req-1", "the request id round-trips");
    CHECK(waits.size() == 0, "a released waiter is removed");
}

void test_wait_timeout_is_not_success() {
    jobs::Registry registry;
    jobs::WaitList waits;
    const std::uint64_t id = registry.accept(reload_request(), 1, 1, at_ms(0));
    jobs::Job started;
    registry.begin_next(at_ms(1), started);
    jobs::Waiter waiter;
    waiter.request_id = "req-2";
    waiter.job_id = id;
    waiter.deadline = at_ms(100);
    waits.add(waiter);
    CHECK(waits.collect(registry, at_ms(99)).empty(), "the wait is still bounded open");
    const std::vector<jobs::WaitList::Ready> ready =
        waits.collect(registry, at_ms(100));
    CHECK(ready.size() == 1 && ready[0].timed_out, "the deadline releases the wait");

    const jobs::Job* job = registry.find(id);
    CHECK(job->state == jobs::State::Running,
          "a timed-out wait does not terminate the job it was waiting on");
    const Value result = jobs::wait_result_json(*job, true, at_ms(100));
    const Value* completed = result.find("completed");
    const Value* timed_out = result.find("timed_out");
    CHECK(completed && completed->kind == Value::Kind::Bool && !completed->b,
          "a timeout never reports completed");
    CHECK(timed_out && timed_out->kind == Value::Kind::Bool && timed_out->b,
          "the timeout is stated");
    CHECK(string_field_is(field(result, "job", "the wait result carries the job"),
                          "state", "running"),
          "the last observed state is reported instead of a guess");
}

void test_wait_list_is_bounded() {
    jobs::WaitList waits;
    for (std::size_t index = 0; index < jobs::kMaxWaiters; ++index) {
        jobs::Waiter waiter;
        waiter.request_id = "req-" + std::to_string(index);
        waiter.job_id = 1;
        waiter.deadline = at_ms(1000);
        CHECK(waits.add(waiter), "waits below the bound are accepted");
    }
    jobs::Waiter overflow;
    overflow.request_id = "too-many";
    overflow.job_id = 1;
    overflow.deadline = at_ms(1000);
    CHECK(!waits.add(overflow), "the wait list is bounded");
}

// --- shutdown ---------------------------------------------------------------

void test_shutdown_fails_live_jobs_and_drains_waits() {
    jobs::Registry registry;
    jobs::WaitList waits;
    const std::uint64_t running = registry.accept(reload_request(), 1, 1, at_ms(0));
    const std::uint64_t queued = registry.accept(regenerate_request(3), 1, 1, at_ms(1));
    const std::uint64_t done = registry.accept(reload_request(), 1, 1, at_ms(2));
    jobs::Job started;
    registry.begin_next(at_ms(3), started);          // `running` runs
    registry.on_bake_started(at_ms(3));
    registry.on_bake_finished(0, 5, 1, false, 0, at_ms(4));  // and completes
    registry.begin_next(at_ms(5), started);          // `queued` runs
    CHECK(started.id == queued, "the queue advanced");
    (void)done;

    jobs::Waiter waiter;
    waiter.request_id = "req-3";
    waiter.job_id = queued;
    waiter.deadline = at_ms(1000000);
    waits.add(waiter);

    registry.shutdown("the editor shut down before this job finished", at_ms(6));
    CHECK(registry.find(running)->state == jobs::State::Completed,
          "shutdown does not rewrite a job that already completed");
    CHECK(registry.find(queued)->state == jobs::State::Failed,
          "a running job fails at shutdown rather than staying 'running' forever");
    CHECK(registry.find(done)->state == jobs::State::Failed,
          "a still-queued job fails at shutdown too");
    CHECK(registry.find(queued)->terminal_reason.find("shut down") != std::string::npos,
          "the shutdown reason is recorded");
    CHECK(registry.running_id() == 0, "nothing runs after shutdown");
    CHECK(waits.drain().size() == 1, "the pending wait is handed back for a terminal record");
    CHECK(waits.size() == 0, "draining empties the list");
}

// --- deterministic seeded regeneration --------------------------------------

void test_seeded_regeneration_is_deterministic() {
    const std::uint64_t a = jobs::request_digest(regenerate_request(42));
    const std::uint64_t b = jobs::request_digest(regenerate_request(42));
    const std::uint64_t c = jobs::request_digest(regenerate_request(43));
    CHECK(a == b, "the same seed against the same world digests equal");
    CHECK(a != c, "a different seed digests differently");
    CHECK(a != jobs::request_digest(reload_request()),
          "a seeded regenerate is not the same request as a plain reload");

    jobs::StartRequest other_world = regenerate_request(42);
    other_world.world = "FloorDemo";
    CHECK(a != jobs::request_digest(other_world),
          "the same seed against another world is a different request");

    // A field separator keeps ("Stream","Mountain") from colliding with
    // ("StreamMountain","").
    jobs::StartRequest split = regenerate_request(42);
    split.project = "projects/world";
    split.world = "_demoStreamMountain";
    CHECK(a != jobs::request_digest(split), "digest fields do not run together");
}

void test_content_digest_is_order_independent() {
    const std::vector<jobs::RootDigest> forward = {
        {"Terrain", 0x1111ull}, {"Trees", 0x2222ull}, {"Rocks", 0x3333ull}};
    const std::vector<jobs::RootDigest> shuffled = {
        {"Rocks", 0x3333ull}, {"Terrain", 0x1111ull}, {"Trees", 0x2222ull}};
    CHECK(jobs::content_digest(forward) == jobs::content_digest(shuffled),
          "part-graph iteration order cannot change the content digest");

    std::vector<jobs::RootDigest> changed = forward;
    changed[0].resolved_hash = 0x1112ull;
    CHECK(jobs::content_digest(forward) != jobs::content_digest(changed),
          "a rebaked root changes the content digest");

    std::vector<jobs::RootDigest> fewer = forward;
    fewer.pop_back();
    CHECK(jobs::content_digest(forward) != jobs::content_digest(fewer),
          "a dropped root changes the content digest");
    CHECK(jobs::content_digest({}) == jobs::content_digest({}),
          "an empty graph digests stably");
}

void test_two_seeded_runs_report_the_same_content() {
    // Two regenerate jobs with the same seed, each completing with the same
    // published roots: the ledger must report the same content digest, which
    // is what makes "deterministic seeded regeneration" checkable end to end.
    const std::uint64_t digest =
        jobs::content_digest({{"Terrain", 0xaaaaull}, {"Trees", 0xbbbbull}});
    jobs::Registry registry;
    std::uint64_t results[2] = {0, 0};
    for (int run = 0; run < 2; ++run) {
        const std::uint64_t id =
            registry.accept(regenerate_request(1234), 1, 1, at_ms(run * 100));
        jobs::Job started;
        registry.begin_next(at_ms(run * 100 + 1), started);
        registry.on_bake_started(at_ms(run * 100 + 2));
        registry.on_bake_finished(0, static_cast<std::uint64_t>(10 + run), 2, true,
                                  digest, at_ms(run * 100 + 50));
        const jobs::Job* job = registry.find(id);
        CHECK(job->state == jobs::State::Completed, "each seeded run completes");
        CHECK(job->has_content_digest, "each run reports a content digest");
        results[run] = job->result_content_digest;
    }
    CHECK(results[0] == results[1],
          "the same seed reports the same resulting content");
    CHECK(registry.find(1)->result_scene_revision !=
              registry.find(2)->result_scene_revision,
          "identical content still carries each run's own scene revision");
}

// --- history bound and listing ----------------------------------------------

void test_history_is_bounded_and_says_so() {
    jobs::Registry registry(4);
    for (int index = 0; index < 6; ++index) {
        const std::uint64_t id =
            registry.accept(reload_request(), 1, 1, at_ms(index * 10));
        jobs::Job started;
        registry.begin_next(at_ms(index * 10 + 1), started);
        registry.on_bake_started(at_ms(index * 10 + 1));
        registry.on_bake_finished(0, static_cast<std::uint64_t>(index), 1, false, 0,
                                  at_ms(index * 10 + 2));
        (void)id;
    }
    CHECK(registry.retained_count() == 4, "history is bounded");
    CHECK(registry.find(1) == nullptr, "the oldest jobs aged out");
    CHECK(registry.find(6) != nullptr, "the newest job is retained");
    CHECK(registry.oldest_retained_id() == 3, "the retained window is reported");
    CHECK(registry.total_accepted() == 6, "the total is not forgotten");

    const Value missing = jobs::missing_result_json(registry, 1);
    CHECK(string_field_is(missing, "reason",
                          "this job has aged out of the retained history window"),
          "an aged-out id is distinguished from one that never existed");
    const Value never = jobs::missing_result_json(registry, 99);
    CHECK(string_field_is(never, "reason",
                          "no job with this id has been accepted in this session"),
          "an id that was never issued says so");
}

void test_live_jobs_are_never_evicted() {
    jobs::Registry registry(2);
    const std::uint64_t live = registry.accept(reload_request(), 1, 1, at_ms(0));
    for (int index = 0; index < 6; ++index) {
        const std::uint64_t id =
            registry.accept(reload_request(), 1, 1, at_ms(10 + index));
        CHECK(registry.cancel(id, at_ms(11 + index)) == jobs::CancelOutcome::Cancelled,
              "the filler jobs terminate");
    }
    CHECK(registry.find(live) != nullptr,
          "a job that never reached a terminal state is never evicted");
    CHECK(registry.find(live)->state == jobs::State::Accepted,
          "and it is still answerable in its real state");
}

void test_list_result_shape() {
    jobs::Registry registry;
    registry.accept(reload_request(), 1, 1, at_ms(0));
    const std::uint64_t second = registry.accept(regenerate_request(9), 1, 1, at_ms(1));
    jobs::Job started;
    registry.begin_next(at_ms(2), started);

    const Value listing = jobs::list_result_json(registry, 10, at_ms(3));
    CHECK(string_field_is(listing, "ordering", "job_id_ascending"),
          "the listing states its total order");
    const Value& rows = field(listing, "jobs", "the listing carries jobs");
    CHECK(rows.kind == Value::Kind::Array && rows.arr.size() == 2, "both jobs list");
    CHECK(string_field_is(rows.arr[0], "job_id", "1") &&
              string_field_is(rows.arr[1], "job_id", "2"),
          "rows are oldest first");
    CHECK(string_field_is(listing, "running_job_id", "1"),
          "the running job is named");
    (void)second;

    const Value bounded = jobs::list_result_json(registry, 1, at_ms(4));
    const Value& bounded_rows = field(bounded, "jobs", "the bounded listing lists");
    CHECK(bounded_rows.arr.size() == 1, "limit bounds the page");
    CHECK(string_field_is(bounded_rows.arr[0], "job_id", "2"),
          "a bounded listing keeps the MOST RECENT jobs");
}

// --- argument parsing -------------------------------------------------------

void test_start_argument_rules() {
    Value arguments;
    arguments.kind = Value::Kind::Object;
    jobs::StartRequest parsed;
    std::string error;

    CHECK(!jobs::parse_start_arguments(arguments, parsed, error) && !error.empty(),
          "operation is required");

    arguments.set("operation", string_value("rebake"));
    CHECK(!jobs::parse_start_arguments(arguments, parsed, error),
          "an unknown operation is invalid_input, not silently a reload");

    arguments.set("operation", string_value("reload"));
    CHECK(jobs::parse_start_arguments(arguments, parsed, error),
          "reload takes no other argument");
    CHECK(parsed.kind == jobs::Kind::Reload && !parsed.has_seed,
          "a reload carries no seed");

    arguments.set("seed", string_value("5"));
    CHECK(!jobs::parse_start_arguments(arguments, parsed, error),
          "a seed on a reload is refused rather than ignored");

    arguments.set("operation", string_value("regenerate"));
    CHECK(jobs::parse_start_arguments(arguments, parsed, error),
          "regenerate takes a seed");
    CHECK(parsed.has_seed && parsed.seed == 5, "the seed parses");

    arguments.set("seed", string_value("18446744073709551615"));
    CHECK(jobs::parse_start_arguments(arguments, parsed, error) &&
              parsed.seed == 18446744073709551615ull,
          "a full 64-bit seed survives, which is why it is a string");

    arguments.set("seed", number_value(5));
    CHECK(!jobs::parse_start_arguments(arguments, parsed, error),
          "a JSON number seed is refused; it would truncate above 2^53");

    arguments.erase("seed");
    CHECK(!jobs::parse_start_arguments(arguments, parsed, error) && !error.empty(),
          "regenerate without a seed is refused; that request is a reload");
}

void test_job_id_and_limit_rules() {
    Value arguments;
    arguments.kind = Value::Kind::Object;
    std::uint64_t id = 0;
    std::string error;
    CHECK(!jobs::parse_job_id(arguments, id, error), "job_id is required");
    arguments.set("job_id", number_value(3));
    CHECK(!jobs::parse_job_id(arguments, id, error),
          "job_id must be a decimal string, matching every other id here");
    arguments.set("job_id", string_value("0"));
    CHECK(!jobs::parse_job_id(arguments, id, error), "job id 0 is never issued");
    arguments.set("job_id", string_value("12"));
    CHECK(jobs::parse_job_id(arguments, id, error) && id == 12, "a valid id parses");

    std::size_t limit = 0;
    Value list_arguments;
    list_arguments.kind = Value::Kind::Object;
    CHECK(jobs::parse_list_limit(list_arguments, limit, error) &&
              limit == jobs::kDefaultListLimit,
          "an omitted limit takes the default");
    list_arguments.set("limit", number_value(0));
    CHECK(!jobs::parse_list_limit(list_arguments, limit, error), "0 is out of range");
    list_arguments.set("limit", number_value(static_cast<double>(jobs::kMaxListLimit + 1)));
    CHECK(!jobs::parse_list_limit(list_arguments, limit, error),
          "over the cap is out of range, not clamped");
    list_arguments.set("limit", number_value(2.5));
    CHECK(!jobs::parse_list_limit(list_arguments, limit, error),
          "a fractional limit is refused");
    list_arguments.set("limit", number_value(7));
    CHECK(jobs::parse_list_limit(list_arguments, limit, error) && limit == 7,
          "an in-range limit parses");
}

// --- terminal status mapping ------------------------------------------------

void test_terminal_status_mapping() {
    CHECK(jobs::wait_status(jobs::State::Completed) == viewer::agent::Status::Ok,
          "only a completed regeneration is ok");
    CHECK(jobs::wait_status(jobs::State::Failed) ==
              viewer::agent::Status::ExecutionFailure,
          "a failed job is execution_failure");
    CHECK(jobs::wait_status(jobs::State::Superseded) ==
              viewer::agent::Status::ExecutionFailure,
          "a superseded job is execution_failure, never ok");
    CHECK(jobs::wait_status(jobs::State::Cancelled) ==
              viewer::agent::Status::ExecutionFailure,
          "a cancelled job is execution_failure, never ok");
    CHECK(jobs::wait_status(jobs::State::Running) !=
              viewer::agent::Status::Ok,
          "a job that never finished cannot report ok");

    jobs::Registry registry;
    const std::uint64_t id = registry.accept(reload_request(), 1, 1, at_ms(0));
    jobs::Job started;
    registry.begin_next(at_ms(1), started);
    registry.on_bake_started(at_ms(1));
    registry.on_bake_error("Rock", "install", jobs::ErrorCode::ScriptError,
                           "ReferenceError at (objects/Rock.js:4:9)");
    registry.on_bake_finished(2, 8, 3, false, 0, at_ms(2));
    const std::string message = jobs::wait_message(*registry.find(id));
    CHECK(message.find("failed") != std::string::npos, "the message names the state");
    CHECK(message.find("objects/Rock.js:4:9") != std::string::npos,
          "the first diagnostic's file:line reaches the terminal message");
    CHECK(jobs::wait_message(*registry.find(id)).empty() == false,
          "a failed job explains itself");

    jobs::Registry clean;
    const std::uint64_t ok_id = clean.accept(reload_request(), 1, 1, at_ms(0));
    clean.begin_next(at_ms(1), started);
    clean.on_bake_started(at_ms(1));
    clean.on_bake_finished(0, 9, 3, false, 0, at_ms(2));
    CHECK(jobs::wait_message(*clean.find(ok_id)).empty(),
          "a completed job has nothing to explain");
}

// --- serialization details --------------------------------------------------

void test_progress_and_timing_shape() {
    jobs::Registry registry;
    const std::uint64_t id = registry.accept(regenerate_request(8), 3, 2, at_ms(0));
    jobs::Job started;
    registry.begin_next(at_ms(10), started);
    registry.on_bake_started(at_ms(12));
    registry.on_part_done("Terrain", "parts", 2, 0);

    const Value record = jobs::job_json(*registry.find(id), at_ms(40));
    const Value& progress = field(record, "progress", "the record carries progress");
    const Value* confirmed = progress.find("engine_confirmed");
    CHECK(confirmed && confirmed->kind == Value::Kind::Bool && confirmed->b,
          "the engine's own BakeStarted is reported once it lands");
    const Value* indeterminate = progress.find("indeterminate");
    CHECK(indeterminate && indeterminate->kind == Value::Kind::Bool &&
              indeterminate->b,
          "total 0 is reported as indeterminate rather than as 0%");
    CHECK(string_field_is(progress, "phase", "parts"), "the phase is reported");

    const Value& inputs = field(record, "inputs", "the record carries its inputs");
    CHECK(string_field_is(inputs, "world_seed", "8"),
          "the seed round-trips as a decimal string");
    const Value* digest = inputs.find("digest");
    CHECK(digest && digest->kind == Value::Kind::String && digest->str.size() == 16,
          "the request digest is a 16-hex-digit string");

    const Value& timing = field(record, "timing", "the record carries timing");
    const Value* queued = timing.find("queued_ms");
    CHECK(queued && queued->kind == Value::Kind::Number && queued->num == 10.0,
          "queued time stops when the job starts");
    const Value* running = timing.find("running_ms");
    CHECK(running && running->kind == Value::Kind::Number && running->num == 30.0,
          "a running job's elapsed time is measured against now");

    const Value not_started =
        jobs::job_json(*registry.find(registry.accept(reload_request(), 1, 1, at_ms(50))),
                       at_ms(60));
    const Value& not_started_timing =
        field(not_started, "timing", "a queued job still carries timing");
    CHECK(is_unavailable(field(not_started_timing, "running_ms",
                               "running_ms is present either way")),
          "a job the engine has not seen reports no running time rather than 0");
}

}  // namespace

int main() {
    test_accept_is_not_completion();
    test_begin_next_runs_one_job();
    test_overlapping_reloads_supersede();
    test_cancellation_error_is_not_the_running_jobs();
    test_external_restart_supersedes();
    test_script_failure_preserves_file_and_line();
    test_source_location_parsing();
    test_diagnostics_are_bounded();
    test_cancel_queued_job_is_real();
    test_cancel_running_job_is_unsupported();
    test_cancel_terminal_and_missing();
    test_wait_releases_on_terminal_state();
    test_wait_timeout_is_not_success();
    test_wait_list_is_bounded();
    test_shutdown_fails_live_jobs_and_drains_waits();
    test_seeded_regeneration_is_deterministic();
    test_content_digest_is_order_independent();
    test_two_seeded_runs_report_the_same_content();
    test_history_is_bounded_and_says_so();
    test_live_jobs_are_never_evicted();
    test_list_result_shape();
    test_terminal_status_mapping();
    test_start_argument_rules();
    test_job_id_and_limit_rules();
    test_progress_and_timing_shape();
    std::printf("Regeneration job tests passed.\n");
    return 0;
}
