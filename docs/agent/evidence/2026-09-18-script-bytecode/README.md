# Built-in Part bytecode reuse

The class-publication path now reuses process-local compiled bytecode for the
immutable built-in Part script. Each call reads that bytecode into a fresh
restricted context and evaluates it normally. No JS object, class, global,
authored default, or runtime crosses between evaluations. The static cache is
initialized thread-safely once, retains debug/source data, and falls back to
source evaluation if cache construction fails. Authored sources and modules
still follow their existing compilation/evaluation paths. Cache identities and
source hashing are unchanged.

Added a regression that mutates a Part class property during initialization
and checks eight fresh hosts each see value 1 and retain the uncached bake
hash. Native script_host_tests passed, including all existing module/particle
fixtures. The first invocation used repo-root cwd and failed shared-lib fixture
lookups; that log is preserved. The authoritative passing invocation used
MatterEngine3/tests as specified by CMake. Native MSVC editor build passed.

Strict terrain audit part-base-bytecode-v1 valid=true, all 2441 prepared sector
keys and 815 geometry cache keys match, zero geometry compilation, 1061 VT hits
and zero misses. Identity worker time: 22692.816 ms (9.297 ms/sector), versus
31236.654 ms (12.797 ms/sector) in prepared-identity-profile-v1, a 27% reduction.
Observed readiness 13.314 s versus that baseline's 15.183 s; earlier runs vary
down to roughly 12 s, so this is not a repeated wall-time guarantee. The
sub-second loading target and sustained sub-10ms frame target remain unmet.

Further shared-module compilation and context setup remain in identity work.
Any extension of bytecode reuse must preserve module-loader resolution, source
change handling, fresh mutable contexts, and exact resolved cache keys.

Full scene logs/traces: C:/tmp/matter-blas-mountain/part-base-bytecode-v1.

## Shared module bytecode reuse

Added a process-local compiled-module cache keyed by exact module name and
source text. It holds at most 512 entries and 32 MiB of accounted source/name/
bytecode payload (map metadata and temporary concurrent readers are additional).
It stores no JSValue or runtime. Compilation and bytecode instantiation occur
outside the mutex, permitting recursive dependency loading. Misses compile
normally; inability to serialize skips caching. Changed transitive sources
continue to link through the current resolved ModuleStore.

Enabled SP2_SCRIPT_HOST in the native shared_lib_tests target, which previously
skipped host-backed import tests. Added closure-state isolation across fresh
contexts, unchanged-parent/changed-child module behavior, stable resolved hashes,
and missing-dependency rejection. Full shared_lib_tests passed. Native editor
build passed. One test relink temporarily failed LNK1168; no test process was
live when inspected and retry linked successfully.

module-bytecode-v1 strict audit valid=true, readiness 12.232 s. All prepared
sector and geometry keys match; zero geometry compilation and all 1061 VT
pages hit with zero misses/errors. Sector identity work totals 13568.956 ms
(5.559 ms/sector), down from built-in-only 22692.816 ms and original 31236.654
ms. Worker timings overlap; this is not equivalent wall-time savings. World
sector fill still takes 6.90 s. Cache I/O/reconstruction/publication remain
substantial and sub-second loading is not achieved.

Full logs and traces: C:/tmp/matter-blas-mountain/module-bytecode-v1.

## Prepared identity request fingerprint

Added ScriptHost::resolve_request_hash for a future prepared identity manifest.
It does not execute JavaScript: it folds exact authored/transitive source bytes,
length-framed raw overrides, sorted child hashes and the existing bake-mode/
engine-version salt. Missing dependencies fail closed. The request key is
separate from the resolved baked-part hash; JSON spelling differences may cause
extra misses and must never be treated as a substitute resolved artifact key.

Native shared_lib_tests passed with new request-key tests covering repeated
identity, changed source, changed raw overrides, child order and child changes,
invalid child ranges, transitive dependency edits, missing dependencies and
source that would throw if evaluated. Initial compile caught FoldResult's
vector<char> representation; corrected to explicit byte pointer/length.

This turn supplies the invalidation key only. No manifest persistence or terrain
lookup shortcut is installed, so no additional runtime speedup is claimed.
Next integration must store request-to-resolved mappings only after successful
normal resolution, validate manifest records, publish atomically in batches,
and fall back to normal resolution on missing/incompatible data. Warm lookup
must still validate the existing prepared-sector policy and dependencies.


## Persistent prepared identity manifest integration

Implemented opt-in request-to-resolved identity storage in prepared_identity_cache.h,
PartStore and the prepared-sector worker. Records are validated, sorted, and
committed every 64 new mappings, with a final partial batch flushed at teardown.
Warm loads still validate prepared-sector policy and dependencies. Native
geometry_hierarchy_tests passed (reopen, conflict, duplicate/schema rejection,
reader snapshot, final partial batch), and the MSVC editor build passed.

Cook: C:/tmp/matter-blas-mountain/identity-manifest-cook-v1, valid=true,
2441 normally resolved identities, no missing prepared/geometry keys.
Warm: C:/tmp/matter-blas-mountain/identity-manifest-warm-v1, valid=true,
2441 manifest hits and zero resolver fallbacks; 815 geometry cache hits,
zero geometry compilations, all 17954 geometry pages published, zero coverage
gaps, and 1061 encoded VT hits with zero misses/errors. No geometry evictions.

Warm full readiness was 10.830 seconds from process start (single instrumented
run; prior no-manifest control 11.664 seconds). Identity work totaled 7302.203 ms
across workers, including request fingerprinting. Geometry read/validation work
was 1357.106 ms and decode work 2173.026 ms; these stages overlap and must not
be summed into wall time. Geometry upload-limit deferrals occurred in 76 frames.
This confirms the warm shortcut works, but neither sub-second loading nor
sustained sub-10ms frames is established. All-camera cache coverage and moving
camera retirement remain separate validation work.


## Memoized request source prefix and world identity host

Split the existing rolling part hash into source-prefix and finalization helpers;
compute_resolved_hash delegates to the same byte stream. Request hashing caches
the framed v1 source prefix per ScriptHost and still folds raw params, children,
current bake-mode salt and version at each request. clear_fold_cache clears the
prefix map too. Sector workers share a dedicated identity-only ScriptHost,
replaced alongside world_sector_source on world installation after workers are
quiesced. Actual JS execution keeps its per-request hosts. No cache recook.

Native shared_lib_tests passed, including an unsplit v1 key equivalence check
and same-host dependency edit invalidation. MSVC editor builds passed.
A first audit with only per-host memoization showed no useful reduction because
sector workers created a fresh host per request (request-prefix-v1, valid=true).
After wiring the world identity host, request-prefix-shared-v1 passed all strict
checks: 2441 manifest hits, 815 geometry hits, zero compilations or missing keys,
17954 geometry pages published, zero coverage gaps, 1061 VT hits with no misses.
Identity aggregate worker time fell from 7322.576 ms (direct-page-adapter-v1) to
150.537 ms. Full readiness was 12.240 s, so no whole-scene improvement proven.
Prepared-sector I/O totaled 2157.061 ms, decode 1446.940 ms, roots 2560.150 ms;
stages overlap. Loading and frame-time acceptance targets remain unmet.

The repeat request-prefix-shared-v2 FAILED at startup in 3.44 seconds with
0xc0000374 (heap corruption), before any geometry/identity cache events. Windows
Application event 1000 at 2026-09-18 20:57:55 names ntdll.dll, offset 0x117eb5,
report aebfb213-da91-466b-a3ba-ab2a8303534e. Last engine log was retopo warm-up;
that is not proof of the corruption origin. A similar earlier VT test failure
was recorded before these edits. Preserve this failure and investigate under
native debugger/page heap; do not report the repeat as performance evidence.
CDB is installed at C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe.

Logs/traces: C:/tmp/matter-blas-mountain/request-prefix-{v1,shared-v1,shared-v2}.
Native build/test logs: /tmp/request-prefix-{build,tests,editor-build}.log and
/tmp/request-prefix-shared-editor-build.log.
