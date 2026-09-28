# Complete fixed-view VT cache and prepared-sector timing

Second cook persisted the additional 25 detail pages. Fresh read-only reopen
vt-complete-warm-v2 passes the strict audit: 1,061 hits, zero misses/errors, no
geometry compilation, 775 admitted geometry assets, coverage 0/0/0. Observed
readiness 12.791 s. This proves coverage for this audited view, not every camera
or every terrain page; deterministic all-required-page cooking remains open.

Added prepared-sector phase timings: mutex waiting, serialized read/open,
source-artifact existence check, archive decode, and geometry-root lookup.
The MSVC editor build passed. prepared-stage-profile-v1 also passes the strict
audit (zero VT misses), readiness 12.118 s. Its 2,441 prepared sector payloads
total 1,606,755,158 bytes; aggregate worker timing totals are saved separately.
Serialized I/O 2,148 ms, mutex waits 2,344 ms, decode 1,446 ms, source checks
125 ms, root lookup 1,997 ms. These overlap across workers.

The enclosing prepared_sector operation reports 35,032 ms aggregate, much more
than these phases. It also calls ScriptHost::resolve_hash, which constructs a
fresh QuickJS runtime/context and evaluates the source/imports to merge static
parameters for each sector. That path is a strong next profiling/optimization
candidate; do not attribute the whole gap to it without measuring separately.
World sector fill remains 7.26 s. One tested camera is not a sub-second load
or sustained sub-10ms completion claim.

Full native logs and traces remain under C:/tmp/matter-blas-mountain with
the run names above. No editor remains running after these audits.

## Direct script identity measurement

Added a prepared_identity timer around sector ScriptHost::resolve_hash.
Native MSVC build passed; prepared-identity-profile-v1 strict audit valid=True,
readiness 15.183 s. All 2,441 identity resolutions
total 31236.654 ms of worker time, mean 12.797 ms.
This directly measures the previously unexplained work before cache lookup;
parallel aggregate time is not wall-clock delay.

Potential next change: cache compiled QuickJS bytecode for immutable source
text while retaining fresh runtimes/contexts and normal evaluation. Module
imports must remain bound through the resolved source set, source changes must
invalidate bytecode, and existing resolved hashes must remain identical. No
bytecode cache is implemented yet; static default values must not be shared
as mutable JS objects across sector evaluations.
