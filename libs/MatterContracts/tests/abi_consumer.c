#include <matter/contracts/abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

matter_geometry_api fake_abi_create(void);
void fake_abi_dispose(matter_geometry_api *);
unsigned fake_abi_live(void);
static unsigned checks = 0;
static void check(int ok, const char *name) {
  ++checks;
  if (!ok) { fprintf(stderr, "FAIL: %s\n", name); exit(1); }
}
typedef struct callbacks { unsigned progress; uint32_t cancelled; } callbacks;
static uint32_t is_cancelled(void *user) { return ((callbacks *)user)->cancelled; }
static void progress(void *user, double fraction, matter_bytes_view stage) {
  callbacks *state = (callbacks *)user;
  check(fraction == 1 && stage.byte_length == 7, "C-ABI callback borrows stage");
  ++state->progress; state->cancelled = 1;
}
int main(void) {
  matter_geometry_api a = fake_abi_create(), b = fake_abi_create();
  matter_bytes_view request = {(const uint8_t *)"recipe", 6}, view = {NULL, 0};
  matter_job_envelope job = {{sizeof(matter_job_envelope), MATTER_ABI_MAJOR, MATTER_ABI_MINOR},
    {(const uint8_t *)"id", 2}, {(const uint8_t *)"rev", 3}, 1};
  matter_budget budget = {{sizeof(matter_budget), MATTER_ABI_MAJOR, MATTER_ABI_MINOR},
    100, 100, 100, 100, 100, 1000000, 100, 10, 1};
  matter_call_result result;
  callbacks state = {0, 0};
  matter_control control = {{sizeof(matter_control), MATTER_ABI_MAJOR, MATTER_ABI_MINOR}, &state, is_cancelled, progress};
  unsigned i;
  check(a.instance && b.instance, "C-ABI independent instances");
  check(matter_check_abi(a.header, sizeof(a)) == MATTER_OK, "C-ABI function table version");
  for (i = 0; i < 1000; ++i) {
    uint8_t copy[3];
    result = a.evaluate(a.instance, request, &job, &budget, NULL);
    check(result.status == MATTER_OK && result.result, "C-ABI owned result");
    check(a.read(a.instance, result.result, &view) == MATTER_OK, "C-ABI immutable view");
    memcpy(copy, view.data, 3);
    check(b.read(b.instance, result.result, &view) == MATTER_INVALID_HANDLE, "C-ABI foreign handle rejected");
    check(a.release(a.instance, result.result) == MATTER_OK, "C-ABI explicit release");
    check(a.release(a.instance, result.result) == MATTER_INVALID_HANDLE, "C-ABI repeated release rejected");
    check(a.read(a.instance, result.result, &view) == MATTER_INVALID_HANDLE, "C-ABI stale view handle rejected");
    check(memcmp(copy, "abc", 3) == 0 && fake_abi_live() == 0, "C-ABI copied bytes survive release without leaks");
  }
  result = a.evaluate(a.instance, request, &job, &budget, &control);
  check(result.status == MATTER_CANCELLED && !result.result && fake_abi_live() == 0 && state.progress == 1,
        "C-ABI callback cancellation publishes nothing");
  job.header.major = 2;
  result = a.evaluate(a.instance, request, &job, &budget, NULL);
  check(result.status == MATTER_UNSUPPORTED_VERSION && !result.result, "C-ABI version failure");
  job.header.major = 1; job.header.size = 1;
  result = a.evaluate(a.instance, request, &job, &budget, NULL);
  check(result.status == MATTER_INVALID_INPUT && !result.result, "C-ABI truncated structure failure");
  job.header.size = sizeof(job); budget.output_bytes = 1;
  result = a.evaluate(a.instance, request, &job, &budget, NULL);
  check(result.status == MATTER_RESOURCE_LIMIT && !result.result, "C-ABI bounded output");
  budget.output_bytes = 100; request.data = (const uint8_t *)"!unknown";
  result = a.evaluate(a.instance, request, &job, &budget, NULL);
  check(result.status == MATTER_UNSUPPORTED_CAPABILITY && !result.result, "C-ABI unknown capability fails");
  request.data = (const uint8_t *)"recipe";
  result = a.evaluate(a.instance, request, &job, &budget, NULL);
  {
    matter_call_result q = a.query(a.instance, result.result, request, &job, &budget, NULL);
    matter_call_result p = a.process(a.instance, result.result, request, &job, &budget, NULL);
    check(q.status == MATTER_OK && p.status == MATTER_OK && fake_abi_live() == 3, "C-ABI staged query/process own distinct handles");
  }
  fake_abi_dispose(&a); fake_abi_dispose(&b);
  check(fake_abi_live() == 0, "C-ABI disposal releases outstanding storage");
  check(a.read(a.instance, result.result, &view) == MATTER_INVALID_HANDLE,
        "C-ABI disposed instance invalidates handles");
  printf("C ABI ownership fixtures: %u checks passed (fake provider only)\n", checks);
  return 0;
}
