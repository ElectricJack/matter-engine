#ifndef MATTER_GEOMETRY_C_H
#define MATTER_GEOMETRY_C_H
#include <matter/contracts/abi.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Production provider of contracts/abi.h. All functions catch C++ exceptions.
 * Each instance is thread-confined; concurrent calls/re-entry are rejected.
 * Separate instances may run concurrently. instance is an opaque token, never
 * a dereferenceable pointer. A copied table does not retain an instance.
 * After disposal all copies, handles and borrowed views are invalid.
 * See README.md for the versioned UTF-8 JSON request/result wire format. */
typedef struct matter_geometry_config {
    matter_abi_header header;
    uint32_t max_results;
    uint64_t max_retained_bytes, max_request_bytes;
    float csg_sample_spacing, csg_reference_radius;
} matter_geometry_config;

/* Initialize header.size/major/minor in caller-owned output structures first.
 * Defaults: 64 live results, 64 MiB retained wire bytes, 8 MiB requests,
 * CSG spacing 0.1 metres, reference radius 0. No implicit unlimited budgets. */
matter_status matter_geometry_default_config(matter_geometry_config *);
matter_status matter_geometry_default_budget(matter_budget *);
/* NULL config selects defaults. On failure output remains untouched. */
matter_status matter_geometry_create(const matter_geometry_config *, matter_geometry_api *);
/* Frees ALL outstanding results; clears instance on success. Double disposal
 * and disposal via a stale table return MATTER_INVALID_HANDLE. */
matter_status matter_geometry_dispose(matter_geometry_api *);

#ifdef __cplusplus
}
#endif
#endif
