#pragma once
#include <matter/contracts/contracts.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace matter::services {
namespace data = matter::contracts;

// Opaque coordinator values. All three must match before publication, including
// retries at the same source revision. Algorithms only echo this envelope.
struct JobEnvelope {
  std::string request_id, source_revision;
  uint64_t generation_epoch = 0;
};
inline bool operator==(const JobEnvelope &a, const JobEnvelope &b) {
  return a.request_id == b.request_id && a.source_revision == b.source_revision &&
         a.generation_epoch == b.generation_epoch;
}
data::ValidationResult validate(const JobEnvelope &);
data::ValidationResult require_capabilities(const std::vector<data::Capability> &required,
                                            const std::vector<data::Capability> &available);

using AssetOwner = std::shared_ptr<const data::AssetDocument>;
using BytesOwner = std::shared_ptr<const std::vector<uint8_t>>;
// A mutable builder must relinquish all mutable aliases before publication.
// Results hold owned immutable data; copying a result retains its storage.
template <class T> class JobResult {
public:
  static JobResult success(JobEnvelope envelope, T value,
                           std::vector<data::Diagnostic> diagnostics = {}) {
    JobResult r(std::move(envelope), std::move(diagnostics));
    if (validate(r.envelope_).ok() && r.diagnostics_ok())
      r.value_ = std::make_shared<const T>(std::move(value));
    else
      r.ensure_error();
    return r;
  }
  static JobResult failure(JobEnvelope envelope, data::ErrorCode code,
                           std::string message) {
    return JobResult(std::move(envelope),
                     {{data::Severity::error, code, std::move(message), {}, {}, {}}});
  }
  static JobResult failure(JobEnvelope envelope, std::vector<data::Diagnostic> diagnostics) {
    JobResult r(std::move(envelope), std::move(diagnostics));
    r.ensure_error();
    return r;
  }
  bool ok() const { return bool(value_); }
  const JobEnvelope &envelope() const { return envelope_; }
  const std::shared_ptr<const T> &value() const { return value_; }
  const std::vector<data::Diagnostic> &diagnostics() const { return diagnostics_; }
private:
  JobResult(JobEnvelope e, std::vector<data::Diagnostic> d)
      : envelope_(std::move(e)), diagnostics_(std::move(d)) {}
  bool diagnostics_ok() const {
    for (const auto &d : diagnostics_)
      if (d.severity == data::Severity::error) return false;
    return true;
  }
  void ensure_error() {
    if (diagnostics_ok()) diagnostics_.push_back(
        {data::Severity::error, data::ErrorCode::invalid_input,
         "invalid result envelope or failure without an error", {}, {}, {}});
  }
  JobEnvelope envelope_;
  std::shared_ptr<const T> value_;
  std::vector<data::Diagnostic> diagnostics_;
};
// Failures and stale results leave the consumer's previous owner untouched.
template <class T>
data::ValidationResult admit(const JobResult<T> &result, const JobEnvelope &expected,
                             std::shared_ptr<const T> &destination) {
  auto valid = validate(expected);
  if (!valid.ok()) return valid;
  if (!(result.envelope() == expected))
    return {{{data::Severity::error, data::ErrorCode::stale_result,
              "request, source revision or generation epoch no longer matches", {}, {}, {}}}};
  if (!result.ok()) return {result.diagnostics()};
  destination = result.value();
  return {result.diagnostics()};
}

class CancellationSource {
public:
  CancellationSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
  void cancel() const noexcept { flag_->store(true, std::memory_order_relaxed); }
  std::shared_ptr<const std::atomic<bool>> token() const { return flag_; }
private:
  std::shared_ptr<std::atomic<bool>> flag_;
};
struct ResourceBudget {
  uint64_t operations = 1000000, vertices = 1000000, indices = 3000000;
  uint64_t output_bytes = 64ull * 1024 * 1024, working_bytes = 64ull * 1024 * 1024;
  uint64_t execution_microseconds = 5000000, queries = 100000;
  uint32_t dependency_depth = 128, progress_events = 64;
};
struct ResourceUsage {
  uint64_t operations = 0, vertices = 0, indices = 0, output_bytes = 0;
  uint64_t working_bytes = 0, queries = 0;
  uint32_t dependency_depth = 0;
};
data::ValidationResult validate(const ResourceBudget &);
struct Progress {
  double fraction = 0;
  std::string stage;
};
struct JobControl {
  std::shared_ptr<const std::atomic<bool>> cancellation;
  // Inline on the calling evaluation thread, bounded by progress_events.
  // Never under a service lock. No re-entry into that service. Exceptions are
  // converted to backend_failure. Captures live through the synchronous call.
  // Async hosts copy/retain this control and inputs through terminal completion;
  // they marshal UI notifications separately and release captures on all exits.
  std::function<void(const Progress &)> progress;
};
// One tracker per synchronous invocation; thread-confined. Usage is cumulative
// except working_bytes/dependency_depth, which are current high-water values.
// Cancellation and deadlines are cooperative: checkpoints surround bounded
// work units and precede return. This is not a hard process timeout.
class JobMonitor {
public:
  JobMonitor(ResourceBudget, JobControl);
  data::ValidationResult checkpoint(const ResourceUsage &);
  data::ValidationResult checkpoint() { return checkpoint(previous_); }
  data::ValidationResult report(Progress);
private:
  ResourceBudget budget_;
  JobControl control_;
  std::chrono::steady_clock::time_point started_;
  ResourceUsage previous_;
  uint32_t reports_ = 0;
  double fraction_ = 0;
  data::ValidationResult terminal_;
};
} // namespace matter::services
