#ifndef MATTER_CONTRACTS_ABI_V1_H
#define MATTER_CONTRACTS_ABI_V1_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Binding contract, not an exported geometry implementation. No C++ types or
 * exceptions cross this boundary. Providers expose a versioned function table.
 * Structures are never packed. v1 requires exact major, minor 0, and size at
 * least the v1 structure size; later additive tails must be capability-gated. */
#define MATTER_ABI_MAJOR 1u
#define MATTER_ABI_MINOR 0u
typedef uint32_t matter_status;
enum {
  MATTER_OK = 0, MATTER_INVALID_INPUT = 1, MATTER_UNSUPPORTED_CAPABILITY = 2,
  MATTER_UNSUPPORTED_VERSION = 3, MATTER_MISSING_DEPENDENCY = 4,
  MATTER_CYCLE = 5, MATTER_RESOURCE_LIMIT = 6, MATTER_CANCELLED = 7,
  MATTER_STALE_RESULT = 8, MATTER_BACKEND_FAILURE = 9, MATTER_INVALID_HANDLE = 10
};
typedef struct matter_abi_header { uint32_t size, major, minor; } matter_abi_header;
static inline matter_status matter_check_abi(matter_abi_header h, uint32_t minimum_size) {
  if (h.major != MATTER_ABI_MAJOR || h.minor != MATTER_ABI_MINOR)
    return MATTER_UNSUPPORTED_VERSION;
  return h.size < minimum_size ? MATTER_INVALID_INPUT : MATTER_OK;
}
typedef struct matter_bytes_view { const uint8_t *data; uint64_t byte_length; } matter_bytes_view;
typedef struct matter_mutable_bytes_view { uint8_t *data; uint64_t byte_length; } matter_mutable_bytes_view;
typedef struct matter_job_envelope {
  matter_abi_header header;
  matter_bytes_view request_id_utf8, source_revision_utf8;
  uint64_t generation_epoch;
} matter_job_envelope;
typedef struct matter_budget {
  matter_abi_header header;
  uint64_t operations, vertices, indices, output_bytes, working_bytes;
  uint64_t execution_microseconds, queries;
  uint32_t dependency_depth, progress_events;
} matter_budget;
typedef uint32_t (*matter_cancelled_fn)(void *user);
typedef void (*matter_progress_fn)(void *user, double fraction, matter_bytes_view stage_utf8);
typedef struct matter_control {
  matter_abi_header header;
  void *user;
  matter_cancelled_fn cancelled;
  matter_progress_fn progress;
} matter_control;
/* Functions are synchronous. Inputs and callback context are borrowed through
 * return. Callbacks run inline on the invoking thread, never under locks; no
 * re-entry. Progress count is bounded by budget; stage bytes live only through
 * callback return. No callback may throw across this ABI. Async wrappers retain
 * inputs/context until terminal completion; hard worker termination invalidates
 * every handle from that worker/library instance. */
typedef uint64_t matter_result_handle; /* Nonzero, instance-scoped, never reused. */
typedef struct matter_call_result {
  matter_abi_header header;
  matter_status status;
  matter_result_handle result;
  /* On every failure result==0: no valid partial output. Diagnostic bytes are
   * copied to caller storage through the provider's diagnostic operation. */
} matter_call_result;
typedef struct matter_geometry_api {
  matter_abi_header header;
  void *instance;
  /* Request wire encoding includes job, algorithm versions, required
   * capabilities and resolved resources. Query/process inputs are result handles
   * from this instance. Unknown required versions/capabilities fail explicitly. */
  matter_call_result (*evaluate)(void *, matter_bytes_view, const matter_job_envelope *,
                                 const matter_budget *, const matter_control *);
  matter_call_result (*query)(void *, matter_result_handle, matter_bytes_view,
                              const matter_job_envelope *, const matter_budget *, const matter_control *);
  matter_call_result (*process)(void *, matter_result_handle, matter_bytes_view,
                                const matter_job_envelope *, const matter_budget *, const matter_control *);
  /* Successful result wire data must echo the complete input job envelope.
   * Library-owned view is immutable and valid until release; caller must copy
   * before release, WASM memory growth or subsequent allocation. No freeing by
   * the caller, sharing handles between instances or double release. */
  matter_status (*read)(void *, matter_result_handle, matter_bytes_view *);
  matter_status (*release)(void *, matter_result_handle);
  matter_status (*capabilities)(void *, matter_mutable_bytes_view caller_buffer, uint64_t *required_bytes);
  matter_status (*diagnostics)(void *, matter_mutable_bytes_view caller_buffer, uint64_t *required_bytes);
} matter_geometry_api;
/* Providers catch all exceptions, own result storage, free it with release on
 * all paths, and invalidate handles at instance disposal. Custom allocators,
 * zero-copy worker transfer and ABI request wire encoding are future binding
 * work, not implemented by this public ownership contract. */
#ifdef __cplusplus
}
#endif
#endif
