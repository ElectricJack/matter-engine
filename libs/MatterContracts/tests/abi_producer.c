#include <matter/contracts/abi.h>
#include <stdlib.h>
#include <string.h>

/* Test producer only: exercises the ABI without implementing geometry. */
typedef struct allocation { matter_result_handle handle; uint8_t *bytes; struct allocation *next; } allocation;
typedef struct provider { uint64_t owner, serial; allocation *results; } provider;
static uint64_t owners = 0;
static unsigned live_allocations = 0;
static matter_call_result outcome(matter_status status, matter_result_handle result) {
  matter_call_result r = {{sizeof(matter_call_result), MATTER_ABI_MAJOR, MATTER_ABI_MINOR}, status, result};
  return r;
}
static allocation *find(provider *p, matter_result_handle handle) {
  allocation *a;
  if (!p) return NULL;
  for (a = p->results; a; a = a->next) if (a->handle == handle) return a;
  return NULL;
}
static matter_call_result evaluate(void *instance, matter_bytes_view request, const matter_job_envelope *job,
                                    const matter_budget *budget, const matter_control *control) {
  provider *p = (provider *)instance;
  matter_status status;
  allocation *a;
  if (!p || !job || !budget) return outcome(MATTER_INVALID_INPUT, 0);
  status = matter_check_abi(job->header, sizeof(*job));
  if (status != MATTER_OK) return outcome(status, 0);
  status = matter_check_abi(budget->header, sizeof(*budget));
  if (status != MATTER_OK) return outcome(status, 0);
  if (control) {
    status = matter_check_abi(control->header, sizeof(*control));
    if (status != MATTER_OK) return outcome(status, 0);
  }
  if (!job->generation_epoch || !job->request_id_utf8.data || !job->request_id_utf8.byte_length ||
      !job->source_revision_utf8.data || !job->source_revision_utf8.byte_length ||
      !request.data || !request.byte_length) return outcome(MATTER_INVALID_INPUT, 0);
  if (request.data[0] == '!') return outcome(MATTER_UNSUPPORTED_CAPABILITY, 0);
  if (!budget->operations || budget->output_bytes < 3) return outcome(MATTER_RESOURCE_LIMIT, 0);
  if (control && control->cancelled && control->cancelled(control->user)) return outcome(MATTER_CANCELLED, 0);
  if (control && control->progress && budget->progress_events) {
    matter_bytes_view stage = {(const uint8_t *)"fixture", 7};
    control->progress(control->user, 1, stage);
    if (control->cancelled && control->cancelled(control->user)) return outcome(MATTER_CANCELLED, 0);
  }
  a = (allocation *)malloc(sizeof(*a));
  if (!a) return outcome(MATTER_BACKEND_FAILURE, 0);
  a->bytes = (uint8_t *)malloc(3);
  if (!a->bytes) { free(a); return outcome(MATTER_BACKEND_FAILURE, 0); }
  memcpy(a->bytes, "abc", 3);
  a->handle = (p->owner << 32) | ++p->serial;
  a->next = p->results; p->results = a; ++live_allocations;
  return outcome(MATTER_OK, a->handle);
}
static matter_call_result query(void *instance, matter_result_handle handle, matter_bytes_view request,
                                 const matter_job_envelope *job, const matter_budget *budget, const matter_control *control) {
  if (!find((provider *)instance, handle)) return outcome(MATTER_INVALID_HANDLE, 0);
  return evaluate(instance, request, job, budget, control);
}
static matter_status read_result(void *instance, matter_result_handle handle, matter_bytes_view *view) {
  allocation *a = find((provider *)instance, handle);
  if (!a || !view) return MATTER_INVALID_HANDLE;
  view->data = a->bytes; view->byte_length = 3;
  return MATTER_OK;
}
static matter_status release_result(void *instance, matter_result_handle handle) {
  provider *p = (provider *)instance;
  if (!p) return MATTER_INVALID_HANDLE;
  allocation **link = &p->results;
  while (*link && (*link)->handle != handle) link = &(*link)->next;
  if (!*link) return MATTER_INVALID_HANDLE;
  {
    allocation *a = *link; *link = a->next;
    free(a->bytes); free(a); --live_allocations;
  }
  return MATTER_OK;
}
static matter_status copy_info(void *instance, matter_mutable_bytes_view buffer, uint64_t *required) {
  (void)instance;
  if (!required) return MATTER_INVALID_INPUT;
  *required = 7;
  if (!buffer.data || buffer.byte_length < *required) return MATTER_RESOURCE_LIMIT;
  memcpy(buffer.data, "fixture", 7);
  return MATTER_OK;
}
matter_geometry_api fake_abi_create(void) {
  provider *p = (provider *)calloc(1, sizeof(provider));
  matter_geometry_api api = {{sizeof(matter_geometry_api), MATTER_ABI_MAJOR, MATTER_ABI_MINOR},
    p, evaluate, query, query, read_result, release_result, copy_info, copy_info};
  if (p) p->owner = ++owners;
  return api;
}
void fake_abi_dispose(matter_geometry_api *api) {
  provider *p = (provider *)api->instance;
  while (p && p->results) release_result(p, p->results->handle);
  free(p); api->instance = NULL;
}
unsigned fake_abi_live(void) { return live_allocations; }
