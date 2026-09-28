# Independent material-module residency

Status: runtime ownership and native sampler/retirement fixture implemented;
focused and regression checks pass. Wall draw mappings, connected POM module lookup,
scene feedback integration and sparse overrides remain open.

## Runtime contract

`VtResidency::acquire_material_module` accepts the immutable snapshot produced by
`vt_make_periodic_material`. Equal complete content identities share one lease
object and registration. Lease copies own the registration independently of any
drawn wall or LOD. The final copy releases pages, the page table, input snapshots
and preparation through the existing GPU retirement mechanism. The cache stores
weak references, so it cannot keep an unused module resident forever.

Modules use a private alias outside the 32 receiver LOD rungs and a separate
parameterisation key. Ordinary part registration, release and surface-edit APIs
cannot mutate or destroy a module. Its direct source program and finite payloads
are independent of mutable scalar material tables, so scalar material edits do
not schedule another module bake. The residency statistics distinguish module
owners and shared acquisitions from total variants.

`material_module_binding` supplies the transported slot and GPU generation only
after the tail is active. CPU validation also checks the complete allocation
generation and runtime epoch. Leases surviving shutdown cannot bind to or release
a restarted runtime's registrations, even if numeric slots are recycled.

Acquisition failure preserves the caller's existing lease. Successful acquisition
creates a candidate; the caller must retain its previous complete published
binding until the candidate is ready. The future wall mapping must retain the
lease along with its mapping and geometry version. A copied slot/generation pair
detects staleness but provides no ownership. This checkpoint does not implement
that wall-level publication transaction.

All acquisition, query and final lease release operations run on the render
thread, like the rest of residency. Runtime shutdown retains its existing caller
contract that GPU work has finished.

## Native fixture

`vt-module-residency` uses the real compositor, asynchronous preparation, page
queue, compressed pool and production material-domain shader. The shader binds
directly to the runtime's descriptors; no synthetic page table or fake successful
fill substitutes for page production. It probes:

- Three consumers sharing one registration, one tail and one preparation.
- No binding before tail production or in its recording frame.
- GPU sampling of the actual tail and detail, including metre-valued height.
- A GPU-produced material request routed through the production feedback queue.
- Repeated resident demand without repeated generation.
- Scalar edits, ordinary part release and attempted in-place source edits.
- Full registration capacity with preservation of the previous active lease.
- Cancellation of unfinished candidates and queue-index integrity.
- Exact GPU pixel retention for already-published bindings until retirement.
- Cleared records at the retirement horizon and a changed generation on reuse.
- Stale GPU token rejection and CPU lease isolation across runtime restart.

The probe uses the module's slot for its synthetic receiver address so it can
exercise GPU reads during retirement. It does not render a wall or validate a
receiver-to-module geometric mapping. The base and finite source are analytic;
the separate [producer evidence](../2026-09-16-periodic-material-producer/README.md)
covers real geometry-baked brick payloads and compressed repeat boundaries.

The focused native check passes with zero Vulkan validation errors. Its three
consumers generate just two pages in the test: one initial tail and one
requested detail page. Subsequent demand generates neither again. The deliberate
capacity-refusal warning is part of the preservation test, not an unexpected
registration failure.

All five MSVC targets build, including the editor, and all eleven native checks
pass: module residency, independent material sampling, chart atlas, CPU residency,
compositor, queue lifetime, composed seams, composed parallax, input snapshots,
direct sources and surface parallax. Vulkan checks report zero validation errors
with no RT skips. The final audit verifies 921 source hashes, all five executable
hashes and every test's executable/log against the frozen build. No frame-time
or scene-memory improvement is claimed by these correctness checks.

Artifacts:

- [Runtime GPU test log](../2026-09-16-shared-vt-pixels/ownership-v1-test-vt-module-residency.log)
- [Build manifest](../2026-09-16-shared-vt-pixels/ownership-v1-build-manifest.json)
- [All native results](../2026-09-16-shared-vt-pixels/ownership-v1-tests.json)
- [Final audit](ownership-v1-final-audit.json)

The next step is to carry retained
module leases and physical mappings into real receiver draws, then use module
height through ordinary and connected POM while keeping receiver coverage and
geometry private. This continues the [material-domain plan](../../../superpowers/plans/2026-09-16-periodic-material-domains.md).

Use a fresh prefix with `../2026-09-16-shared-vt-pixels/build_checks.py` to freeze
and build the native sources, then pass that prefix and `vt-module-residency` to
the sibling `run_checks.py`. The test is selected by
`MATTER_VK_SMOKE_MODE=vt-module-residency` in `vulkan_smoke_tests.exe`.
