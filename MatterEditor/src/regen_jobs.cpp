// MatterEditor/src/regen_jobs.cpp
//
// Implementation of the regeneration job ledger declared in regen_jobs.h. The
// ordering rules that matter are all in one place here:
//
//   * begin_next() is the ONLY transition into `running`, and it supersedes
//     the previous running job in the same call. Supersession is therefore a
//     recorded decision, not an inference from a later cancellation event.
//   * every on_*() ingestion applies to the running job and to nothing else.
//     An event that arrives with no running job (the initial startup bake, the
//     deferred tileset phase after a BakeFinished, a world switch's own bake)
//     is dropped rather than attributed to whichever job happens to be newest.
//   * finish() is the ONLY transition into a terminal state, so "terminal
//     exactly once" and "finished_at is always set on a terminal job" hold by
//     construction.

#include "regen_jobs.h"

#include "agent_protocol.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <utility>

namespace viewer::jobs {
namespace {

using matter::jsondoc::Value;
using agent::json::array;
using agent::json::boolean;
using agent::json::decimal;
using agent::json::number;
using agent::json::object;
using agent::json::string;
using agent::json::unavailable;

std::uint64_t fnv1a64(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

Value hex_hash(std::uint64_t hash) {
    char text[17] = {};
    std::snprintf(text, sizeof(text), "%016llx",
                  static_cast<unsigned long long>(hash));
    return string(text);
}

// Milliseconds between two steady-clock points, floored at zero. `now` is the
// caller's frame time and a terminal job's finished_at is always <= it, but a
// negative duration would print as a huge unsigned number if it ever were not.
std::uint64_t elapsed_ms(Clock::time_point from, Clock::time_point to) {
    if (to <= from) return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
}

bool decimal_u64(const std::string& text, std::uint64_t& out) {
    if (text.empty() || text.size() > 20) return false;
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10ull)
            return false;
        value = value * 10ull + digit;
    }
    out = value;
    return true;
}

// One field, rejecting a duplicated key the same way the protocol boundary
// does elsewhere: a strict-JSON document cannot contain one, but an argument
// object built by a caller can, and silently taking the first is how two
// readers end up disagreeing about what was requested.
const Value* argument(const Value& arguments, const char* key) {
    if (arguments.kind != Value::Kind::Object) return nullptr;
    return arguments.find(key);
}

// [begin, end) digits as an int, clamped at a million lines/columns rather than
// overflowing. False only when the run is empty.
bool bounded_int(const std::string& text, std::size_t begin, std::size_t end,
                 int& out) {
    if (begin >= end) return false;
    long long value = 0;
    for (std::size_t at = begin; at < end; ++at) {
        value = value * 10 + (text[at] - '0');
        if (value > 1000000) { value = 1000000; break; }
    }
    out = static_cast<int>(value);
    return true;
}

bool is_path_byte(char c) {
    return !(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' ||
             c == ')' || c == '"' || c == '\'' || c == ',' || c == ';');
}

}  // namespace

const char* to_string(Kind kind) {
    switch (kind) {
        case Kind::Reload: return "reload";
        case Kind::Regenerate: return "regenerate";
        case Kind::Parameters: return "parameters";
    }
    return "reload";
}

const char* to_string(State state) {
    switch (state) {
        case State::Accepted: return "accepted";
        case State::Running: return "running";
        case State::Completed: return "completed";
        case State::Failed: return "failed";
        case State::Cancelled: return "cancelled";
        case State::Superseded: return "superseded";
    }
    return "accepted";
}

const char* to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::None: return "none";
        case ErrorCode::Cancelled: return "cancelled";
        case ErrorCode::OutOfMemory: return "out_of_memory";
        case ErrorCode::ScriptError: return "script_error";
        case ErrorCode::GpuError: return "gpu_error";
        case ErrorCode::IoError: return "io_error";
        case ErrorCode::Internal: return "internal";
    }
    return "internal";
}

bool is_terminal(State state) {
    return state == State::Completed || state == State::Failed ||
           state == State::Cancelled || state == State::Superseded;
}

// Scans for the first "<something>.js:<line>[:<col>]". QuickJS puts the
// innermost frame first in a stack, so the first match is the line that threw.
// The path is walked BACKWARDS from the extension to the first byte that
// cannot be part of a path, which is what makes "    at build (objects/Rock.js:88:12)"
// yield "objects/Rock.js" rather than "at build (objects/Rock.js".
bool parse_source_location(const std::string& text, std::string& file, int& line,
                           int& column) {
    static const char* kExtensions[] = {".js:", ".mjs:", ".cjs:"};
    for (std::size_t at = 0; at < text.size(); ++at) {
        std::size_t extension_length = 0;
        for (const char* extension : kExtensions) {
            const std::size_t length = std::char_traits<char>::length(extension);
            if (text.compare(at, length, extension) == 0) {
                extension_length = length;
                break;
            }
        }
        if (extension_length == 0) continue;

        std::size_t digits = at + extension_length;
        std::size_t line_end = digits;
        while (line_end < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[line_end])))
            ++line_end;
        if (line_end == digits) continue;  // ".js:" with no line number

        std::size_t start = at;
        while (start > 0 && is_path_byte(text[start - 1])) --start;
        if (start == at) continue;  // no path characters before the extension

        int parsed_line = 0;
        int parsed_column = 0;
        // Hand-rolled rather than std::stol: a pathological digit run is a
        // saturating clamp here, not a thrown exception on a diagnostic path.
        if (!bounded_int(text, digits, line_end, parsed_line)) continue;
        if (line_end < text.size() && text[line_end] == ':') {
            std::size_t column_end = line_end + 1;
            while (column_end < text.size() &&
                   std::isdigit(static_cast<unsigned char>(text[column_end])))
                ++column_end;
            if (column_end > line_end + 1)
                bounded_int(text, line_end + 1, column_end, parsed_column);
        }
        file = text.substr(start, (at + extension_length - 1) - start);
        line = parsed_line;
        column = parsed_column;
        return true;
    }
    return false;
}

std::uint64_t request_digest(const StartRequest& request) {
    // Field-separated so ("a", "bc") and ("ab", "c") cannot digest equal.
    std::string canonical = std::string(to_string(request.kind));
    canonical += '\x1f';
    canonical += request.project;
    canonical += '\x1f';
    canonical += request.world;
    canonical += '\x1f';
    canonical += request.has_seed ? ("seed=" + std::to_string(request.seed))
                                  : std::string("seed=none");
    canonical += '\x1f';
    canonical += request.parameter_module;
    canonical += '\x1f';
    canonical += request.parameters_json;
    return fnv1a64(canonical);
}

std::uint64_t content_digest(std::vector<RootDigest> roots) {
    std::sort(roots.begin(), roots.end(),
              [](const RootDigest& a, const RootDigest& b) {
                  if (a.module != b.module) return a.module < b.module;
                  return a.resolved_hash < b.resolved_hash;
              });
    std::string canonical;
    for (const RootDigest& root : roots) {
        canonical += root.module;
        canonical += '\x1f';
        canonical += std::to_string(root.resolved_hash);
        canonical += '\x1e';
    }
    return fnv1a64(canonical);
}

// --- Registry ---------------------------------------------------------------

Registry::Registry(std::size_t history_limit)
    : history_limit_(history_limit == 0 ? 1 : history_limit) {}

std::uint64_t Registry::accept(StartRequest request, std::uint64_t scene_revision,
                               std::uint64_t scene_generation,
                               Clock::time_point now) {
    Job job;
    job.id = next_id_++;
    job.state = State::Accepted;
    job.digest = request_digest(request);
    job.request = std::move(request);
    job.accepted_scene_revision = scene_revision;
    job.accepted_scene_generation = scene_generation;
    job.accepted_at = now;
    const std::uint64_t id = job.id;
    jobs_.push_back(std::move(job));
    queue_.push_back(id);
    trim();
    return id;
}

Job* Registry::mutable_find(std::uint64_t id) {
    for (Job& job : jobs_)
        if (job.id == id) return &job;
    return nullptr;
}

Job* Registry::running_job() {
    return running_ == 0 ? nullptr : mutable_find(running_);
}

void Registry::finish(Job& job, State state, const std::string& reason,
                      Clock::time_point now) {
    if (is_terminal(job.state)) return;
    job.state = state;
    job.terminal_reason = reason;
    job.has_finished = true;
    job.finished_at = now;
    if (running_ == job.id) running_ = 0;
}

bool Registry::begin_next(Clock::time_point now, Job& started) {
    while (!queue_.empty()) {
        const std::uint64_t id = queue_.front();
        queue_.pop_front();
        Job* job = mutable_find(id);
        // A cancelled job leaves its id in the queue; skipping it here is what
        // makes cancel() O(1) without a second structure to keep in step.
        if (!job || job->state != State::Accepted) continue;

        if (Job* previous = running_job()) {
            previous->superseded_by = id;
            finish(*previous,
                   State::Superseded,
                   "superseded by job " + std::to_string(id) +
                       "; the engine cancels an in-flight bake when a newer "
                       "reload/regenerate is requested",
                   now);
        }
        job->state = State::Running;
        job->has_started = true;
        job->started_at = now;
        running_ = id;
        started = *job;
        return true;
    }
    return false;
}

void Registry::note_external_restart(const std::string& reason,
                                     Clock::time_point now) {
    if (Job* previous = running_job()) {
        previous->superseded_by = 0;
        finish(*previous, State::Superseded, reason, now);
    }
}

void Registry::on_bake_started(Clock::time_point now) {
    (void)now;
    Job* job = running_job();
    if (!job) return;
    // `started_at` is deliberately NOT moved to this moment. The job started
    // when the seam handed it to the engine; BakeStarted only confirms the
    // engine picked it up, and rewriting the start here would hide the handoff
    // latency inside `running_ms` instead of leaving it in `queued_ms`. What
    // this does own is opening the job to progress and completion (rule 1 of
    // the ATTRIBUTION note) and clearing progress a previous phase left behind.
    job->engine_started = true;
    job->parts_done = 0;
    job->parts_total = 0;
    job->phase.clear();
    job->module.clear();
}

void Registry::on_part_done(const std::string& module, const std::string& phase,
                            int done, int total) {
    Job* job = running_job();
    if (!job || !job->engine_started) return;
    job->parts_done = done;
    job->parts_total = total;
    job->module = module;
    job->phase = phase;
}

void Registry::on_bake_error(const std::string& module, const std::string& phase,
                             ErrorCode code, const std::string& message) {
    Job* job = running_job();
    if (!job) return;
    // Rule 2 of the ATTRIBUTION note: a cancellation is a SUPERSEDED bake's
    // exit notice. This ledger records supersession at the seam, so the
    // running job is by construction not the one being cancelled.
    if (code == ErrorCode::Cancelled) return;
    if (job->diagnostics.size() >= kMaxDiagnostics) {
        job->diagnostics_truncated = true;
        return;
    }
    Diagnostic diagnostic;
    diagnostic.module = module;
    diagnostic.phase = phase;
    diagnostic.code = code;
    diagnostic.message = message;
    diagnostic.has_source = parse_source_location(
        message, diagnostic.source_file, diagnostic.source_line,
        diagnostic.source_column);
    job->diagnostics.push_back(std::move(diagnostic));
}

void Registry::on_bake_finished(int errors, std::uint64_t scene_revision,
                                std::uint64_t scene_generation,
                                bool has_content_digest,
                                std::uint64_t digest, Clock::time_point now) {
    Job* job = running_job();
    // Rule 1 of the ATTRIBUTION note: a completion that arrives before this
    // job's own BakeStarted belongs to the bake it superseded. Dropping it is
    // the whole point -- taking it would report a predecessor's result, with
    // this job's id on it, as a success.
    if (!job || !job->engine_started) return;
    job->failed_parts = errors;
    job->has_result_scene = true;
    job->result_scene_revision = scene_revision;
    job->result_scene_generation = scene_generation;
    job->has_content_digest = has_content_digest;
    job->result_content_digest = has_content_digest ? digest : 0;
    if (errors == 0) {
        finish(*job, State::Completed, {}, now);
    } else {
        finish(*job, State::Failed,
               std::to_string(errors) + " part(s) failed to bake", now);
    }
}

CancelOutcome Registry::cancel(std::uint64_t id, Clock::time_point now) {
    Job* job = mutable_find(id);
    if (!job) return CancelOutcome::NotFound;
    if (is_terminal(job->state)) return CancelOutcome::AlreadyTerminal;
    if (job->state == State::Running) return CancelOutcome::UnsupportedRunning;
    finish(*job, State::Cancelled,
           "cancelled by job.cancel before the editor handed it to the engine",
           now);
    return CancelOutcome::Cancelled;
}

void Registry::shutdown(const std::string& reason, Clock::time_point now) {
    for (Job& job : jobs_)
        if (!is_terminal(job.state)) finish(job, State::Failed, reason, now);
    queue_.clear();
    running_ = 0;
}

const Job* Registry::find(std::uint64_t id) const {
    for (const Job& job : jobs_)
        if (job.id == id) return &job;
    return nullptr;
}

std::vector<const Job*> Registry::recent(std::size_t limit) const {
    std::vector<const Job*> out;
    if (limit == 0) return out;
    const std::size_t count = std::min(limit, jobs_.size());
    out.reserve(count);
    for (std::size_t index = jobs_.size() - count; index < jobs_.size(); ++index)
        out.push_back(&jobs_[index]);
    return out;
}

std::uint64_t Registry::oldest_retained_id() const {
    return jobs_.empty() ? 0 : jobs_.front().id;
}

// Drops the oldest TERMINAL entries only. A live job is never evicted, so a
// caller holding a job id can always still be told what happened to it; the
// bound is on history, not on work in flight.
void Registry::trim() {
    while (jobs_.size() > history_limit_) {
        auto oldest = std::find_if(jobs_.begin(), jobs_.end(), [](const Job& job) {
            return is_terminal(job.state);
        });
        if (oldest == jobs_.end()) return;
        jobs_.erase(oldest);
    }
}

// --- WaitList ---------------------------------------------------------------

bool WaitList::add(Waiter waiter) {
    if (waiters_.size() >= kMaxWaiters) return false;
    waiters_.push_back(std::move(waiter));
    return true;
}

std::vector<WaitList::Ready> WaitList::collect(const Registry& registry,
                                               Clock::time_point now) {
    std::vector<Ready> ready;
    std::vector<Waiter> keep;
    keep.reserve(waiters_.size());
    for (Waiter& waiter : waiters_) {
        const Job* job = registry.find(waiter.job_id);
        if (job && is_terminal(job->state)) {
            ready.push_back(Ready{std::move(waiter), false});
            continue;
        }
        // A job that aged out of the retained window cannot be waited on any
        // longer; the deadline is what releases that waiter, and it is
        // reported as a timeout rather than as a completion.
        if (now >= waiter.deadline) {
            ready.push_back(Ready{std::move(waiter), true});
            continue;
        }
        keep.push_back(std::move(waiter));
    }
    waiters_ = std::move(keep);
    return ready;
}

std::vector<Waiter> WaitList::drain() {
    std::vector<Waiter> out = std::move(waiters_);
    waiters_.clear();
    return out;
}

// --- terminal mapping -------------------------------------------------------

agent::Status wait_status(State state) {
    switch (state) {
        case State::Completed: return agent::Status::Ok;
        case State::Failed:
        case State::Cancelled:
        case State::Superseded: return agent::Status::ExecutionFailure;
        // Not reachable from a released wait (collect() only hands back
        // terminal jobs and expiries), but a non-terminal job is emphatically
        // not a success if one ever gets here.
        case State::Accepted:
        case State::Running: return agent::Status::ExecutionFailure;
    }
    return agent::Status::ExecutionFailure;
}

std::string wait_message(const Job& job) {
    if (job.state == State::Completed) return {};
    std::string message = "job " + std::to_string(job.id) + " ended in state '" +
                          to_string(job.state) + "'";
    if (!job.terminal_reason.empty()) message += ": " + job.terminal_reason;
    if (!job.diagnostics.empty()) {
        const Diagnostic& first = job.diagnostics.front();
        message += " (first diagnostic: ";
        if (first.has_source) {
            message += first.source_file + ":" + std::to_string(first.source_line);
            if (first.source_column > 0)
                message += ":" + std::to_string(first.source_column);
            message += " ";
        }
        message += first.message + ")";
    }
    return message;
}

// --- serialization ----------------------------------------------------------

namespace {

Value diagnostic_json(const Diagnostic& diagnostic) {
    Value out = object();
    out.set("module", diagnostic.module.empty()
                          ? unavailable("the engine reported no module for this error")
                          : string(diagnostic.module));
    out.set("phase", diagnostic.phase.empty()
                         ? unavailable("the engine reported no bake phase for this error")
                         : string(diagnostic.phase));
    out.set("code", string(to_string(diagnostic.code)));
    out.set("message", string(diagnostic.message));
    if (diagnostic.has_source) {
        Value source = object();
        source.set("available", boolean(true));
        source.set("file", string(diagnostic.source_file));
        source.set("line", number(diagnostic.source_line));
        if (diagnostic.source_column > 0)
            source.set("column", number(diagnostic.source_column));
        else
            source.set("column",
                       unavailable("the diagnostic named a line but no column"));
        out.set("source", std::move(source));
    } else {
        out.set("source",
                unavailable("the engine message carried no script file:line"));
    }
    return out;
}

Value inputs_json(const Job& job) {
    Value inputs = object();
    inputs.set("operation", string(to_string(job.request.kind)));
    inputs.set("world", job.request.world.empty()
                            ? unavailable("no world was open when the job was accepted")
                            : string(job.request.world));
    inputs.set("project", job.request.project.empty()
                              ? unavailable("no project directory was recorded")
                              : string(job.request.project));
    inputs.set("world_seed",
               job.request.has_seed
                   ? decimal(job.request.seed)
                   : unavailable("this operation carries no seed override; the "
                                 "world's authored seed is used"));
    inputs.set("parameter_module",
               job.request.kind == Kind::Parameters
                   ? string(job.request.parameter_module)
                   : unavailable("this operation carries no parameter override"));
    inputs.set("parameters_json",
               job.request.kind == Kind::Parameters
                   ? string(job.request.parameters_json)
                   : unavailable("this operation carries no parameter override"));
    inputs.set("digest", hex_hash(job.digest));
    inputs.set("digest_algorithm", string("fnv1a64_operation_project_world_seed_parameters"));
    return inputs;
}

Value result_json(const Job& job) {
    Value result = object();
    if (job.has_result_scene) {
        result.set("scene_revision", decimal(job.result_scene_revision));
        result.set("scene_generation", decimal(job.result_scene_generation));
    } else {
        Value reason = unavailable(
            "this job published no BakeFinished, so it produced no scene revision");
        result.set("scene_revision", reason);
        result.set("scene_generation", std::move(reason));
    }
    if (job.has_content_digest) {
        result.set("content_digest", hex_hash(job.result_content_digest));
        result.set("content_digest_algorithm",
                   string("fnv1a64_sorted_root_module_hash"));
    } else {
        result.set("content_digest",
                   unavailable("this job published no part-graph roots to digest; "
                               "a world-kind (streamed) session installs sector "
                               "assets and publishes none"));
    }
    return result;
}

}  // namespace

Value job_json(const Job& job, Clock::time_point now) {
    Value out = object();
    out.set("job_id", decimal(job.id));
    out.set("state", string(to_string(job.state)));
    out.set("terminal", boolean(is_terminal(job.state)));
    out.set("inputs", inputs_json(job));

    Value accepted = object();
    accepted.set("scene_revision", decimal(job.accepted_scene_revision));
    accepted.set("scene_generation", decimal(job.accepted_scene_generation));
    out.set("accepted_context", std::move(accepted));

    Value progress = object();
    // Whether the engine has actually confirmed this job's bake with its own
    // BakeStarted. Reported because it is what gates progress and completion
    // (ATTRIBUTION rule 1), and because a job stuck true-but-never-finishing is
    // the shape of the aborting-bake limit documented in regen_jobs.h.
    progress.set("engine_confirmed", boolean(job.engine_started));
    progress.set("parts_done", number(job.parts_done));
    // matter/events.h: total 0 means indeterminate and total may GROW mid-bake,
    // so this is labelled advisory rather than presented as a denominator.
    progress.set("parts_total", number(job.parts_total));
    progress.set("indeterminate", boolean(job.parts_total == 0));
    progress.set("advisory", boolean(true));
    progress.set("phase", job.phase.empty()
                              ? unavailable("no bake phase has been reported yet")
                              : string(job.phase));
    progress.set("module", job.module.empty()
                               ? unavailable("no part module has been reported yet")
                               : string(job.module));
    progress.set("failed_parts", number(job.failed_parts));
    out.set("progress", std::move(progress));

    out.set("result", result_json(job));

    Value diagnostics = array();
    diagnostics.arr.reserve(job.diagnostics.size());
    for (const Diagnostic& diagnostic : job.diagnostics)
        diagnostics.arr.push_back(diagnostic_json(diagnostic));
    out.set("diagnostics", std::move(diagnostics));
    out.set("diagnostics_truncated", boolean(job.diagnostics_truncated));

    out.set("superseded_by", job.superseded_by != 0
                                 ? decimal(job.superseded_by)
                                 : unavailable("no later job superseded this one"));
    out.set("terminal_reason",
            job.terminal_reason.empty()
                ? unavailable(is_terminal(job.state)
                                  ? "the job completed with nothing to report"
                                  : "the job has not reached a terminal state")
                : string(job.terminal_reason));

    Value timing = object();
    timing.set("queued_ms",
               number(static_cast<double>(elapsed_ms(
                   job.accepted_at, job.has_started ? job.started_at : now))));
    if (job.has_started) {
        timing.set("running_ms",
                   number(static_cast<double>(elapsed_ms(
                       job.started_at, job.has_finished ? job.finished_at : now))));
    } else {
        timing.set("running_ms",
                   unavailable("the editor has not handed this job to the engine yet"));
    }
    timing.set("total_ms",
               number(static_cast<double>(elapsed_ms(
                   job.accepted_at, job.has_finished ? job.finished_at : now))));
    out.set("timing", std::move(timing));
    return out;
}

Value start_result_json(const Job& job, Clock::time_point now) {
    Value out = object();
    out.set("accepted", boolean(true));
    out.set("job", job_json(job, now));
    // Said plainly, because "the request succeeded" and "the world regenerated"
    // are exactly the two things an agent must not conflate here.
    out.set("note",
            string("accepted only; the regeneration has not run yet. Poll "
                   "job.status or take a bounded job.wait."));
    return out;
}

Value status_result_json(const Job& job, Clock::time_point now) {
    Value out = object();
    out.set("job", job_json(job, now));
    return out;
}

Value wait_result_json(const Job& job, bool timed_out, Clock::time_point now) {
    Value out = object();
    out.set("timed_out", boolean(timed_out));
    out.set("completed", boolean(!timed_out && job.state == State::Completed));
    out.set("job", job_json(job, now));
    return out;
}

Value cancel_result_json(const Job& job, CancelOutcome outcome,
                         Clock::time_point now) {
    Value out = object();
    Value cancel = object();
    switch (outcome) {
        case CancelOutcome::Cancelled:
            cancel.set("supported", boolean(true));
            cancel.set("cancelled", boolean(true));
            cancel.set("reason",
                       string("the job was still queued in the editor, so it was "
                              "dropped before the engine ever saw it"));
            break;
        case CancelOutcome::UnsupportedRunning:
            cancel.set("supported", boolean(false));
            cancel.set("cancelled", boolean(false));
            cancel.set("reason",
                       string("WorldSession exposes no cancel for a bake that is "
                              "already running; supersession is the only "
                              "mechanism the backend has"));
            cancel.set("alternative",
                       string("start a newer job with job.start; it supersedes "
                              "this one and this job reports state=superseded"));
            break;
        case CancelOutcome::AlreadyTerminal:
            cancel.set("supported", boolean(true));
            cancel.set("cancelled", boolean(false));
            cancel.set("reason", string("the job had already reached state '" +
                                        std::string(to_string(job.state)) + "'"));
            break;
        case CancelOutcome::NotFound:
            // Never serialized: a missing job answers missing_result_json.
            cancel.set("supported", boolean(false));
            cancel.set("cancelled", boolean(false));
            cancel.set("reason", string("no such job"));
            break;
    }
    out.set("cancel", std::move(cancel));
    out.set("job", job_json(job, now));
    return out;
}

Value list_result_json(const Registry& registry, std::size_t limit,
                       Clock::time_point now) {
    Value out = object();
    const std::vector<const Job*> rows = registry.recent(limit);
    Value jobs = array();
    jobs.arr.reserve(rows.size());
    for (const Job* job : rows) jobs.arr.push_back(job_json(*job, now));
    out.set("ordering", string("job_id_ascending"));
    out.set("jobs", std::move(jobs));

    Value page = object();
    page.set("limit", number(static_cast<double>(limit)));
    page.set("returned", number(static_cast<double>(rows.size())));
    page.set("retained", number(static_cast<double>(registry.retained_count())));
    out.set("page", std::move(page));

    out.set("total_accepted", decimal(registry.total_accepted()));
    out.set("oldest_retained_job_id",
            registry.oldest_retained_id() != 0
                ? decimal(registry.oldest_retained_id())
                : unavailable("no jobs have been accepted in this session"));
    out.set("running_job_id",
            registry.running_id() != 0
                ? decimal(registry.running_id())
                : unavailable("no regeneration job is running"));
    return out;
}

Value missing_result_json(const Registry& registry, std::uint64_t id) {
    Value out = object();
    out.set("found", boolean(false));
    out.set("job_id", decimal(id));
    out.set("total_accepted", decimal(registry.total_accepted()));
    out.set("oldest_retained_job_id",
            registry.oldest_retained_id() != 0
                ? decimal(registry.oldest_retained_id())
                : unavailable("no jobs have been accepted in this session"));
    out.set("reason",
            string(id != 0 && id <= registry.total_accepted()
                       ? "this job has aged out of the retained history window"
                       : "no job with this id has been accepted in this session"));
    return out;
}

// --- argument parsing -------------------------------------------------------

bool parse_start_arguments(const Value& arguments, StartRequest& out,
                           std::string& error) {
    const Value* operation = argument(arguments, "operation");
    if (!operation || operation->kind != Value::Kind::String) {
        error = "operation is required and must be \"reload\" or \"regenerate\"";
        return false;
    }
    StartRequest parsed;
    if (operation->str == "reload") {
        parsed.kind = Kind::Reload;
    } else if (operation->str == "regenerate") {
        parsed.kind = Kind::Regenerate;
    } else {
        error = "operation '" + operation->str +
                "' is not one of \"reload\", \"regenerate\"";
        return false;
    }

    const Value* seed = argument(arguments, "seed");
    if (seed) {
        if (parsed.kind != Kind::Regenerate) {
            // Silently ignoring it would make a caller believe a reload
            // rerolled the world.
            error = "seed applies to operation \"regenerate\" only; a reload "
                    "reuses the world's authored seed";
            return false;
        }
        std::uint64_t value = 0;
        if (seed->kind != Value::Kind::String || !decimal_u64(seed->str, value)) {
            error = "seed must be an unsigned 64-bit decimal STRING, so it "
                    "survives an IEEE-754 client";
            return false;
        }
        parsed.has_seed = true;
        parsed.seed = value;
    } else if (parsed.kind == Kind::Regenerate) {
        error = "operation \"regenerate\" requires seed; a reroll with no seed "
                "is a reload";
        return false;
    }
    out = std::move(parsed);
    return true;
}

bool parse_job_id(const Value& arguments, std::uint64_t& out, std::string& error) {
    const Value* value = argument(arguments, "job_id");
    if (!value || value->kind != Value::Kind::String) {
        error = "job_id is required and must be the decimal string job.start returned";
        return false;
    }
    std::uint64_t parsed = 0;
    if (!decimal_u64(value->str, parsed) || parsed == 0) {
        error = "job_id must be a non-zero unsigned 64-bit decimal string";
        return false;
    }
    out = parsed;
    return true;
}

bool parse_list_limit(const Value& arguments, std::size_t& out, std::string& error) {
    const Value* value = argument(arguments, "limit");
    if (!value) {
        out = kDefaultListLimit;
        return true;
    }
    double numeric = 0.0;
    if (value->kind == Value::Kind::UInt64) {
        numeric = static_cast<double>(value->uint64_value);
    } else if (value->kind == Value::Kind::Number) {
        numeric = value->num;
    } else {
        error = "limit must be an integer between 1 and " +
                std::to_string(kMaxListLimit);
        return false;
    }
    if (!(numeric >= 1.0) || numeric > static_cast<double>(kMaxListLimit) ||
        numeric != static_cast<double>(static_cast<long long>(numeric))) {
        error = "limit must be an integer between 1 and " +
                std::to_string(kMaxListLimit);
        return false;
    }
    out = static_cast<std::size_t>(numeric);
    return true;
}

}  // namespace viewer::jobs
