# Shared material receivers: coverage-only pages

Status: seven focused native checks pass, including CPU residency, receiver
mapping, compositor, raster/RT parallax, export and connected seams.
This is a checkpoint toward the existing wall/layer/
terrain goal, not visual or performance acceptance.

## Change

Mapped planar wall interiors can keep their own categorical coverage and geometry
without producing another copy of the module's color, normal, ORM and height.
The whole stored page, including its four-texel gutters, must fit one supported
chart and the finite material interval. Mixed carriers, uncertain geometry,
boundaries and mandatory coarse tails keep the ordinary complete producer.

The compositor skips base evaluation, BC encoding and material copies for these
pages. Residency retains their geometry, source snapshot and immutable module
mapping but allocates no private material slot. `coverage_only_pages` counts live
receiver pages; `coverage_pages_filled` counts successful compositor outputs.
The pool's configured reservation is unchanged. These counters measure avoided
occupied material pages/work, not a reduction in allocated GPU memory.

Removing or narrowing a mapping first rebuilds incompatible receiver pages with
complete finite materials while the old table remains visible. Only then may
the new table publish. Failed scratch writes must not alter the old resident
material. Module leases remain alive through page retirement. Enabling receiver
enrichment requests full pages again before that stage may modify their ORM.

## Checks

Native evidence uses the `coverage-*` prefixes in
[shared VT evidence](../2026-09-16-shared-vt-pixels/). The build harness records
source and executable hashes; validation must match those exact sources.

- CPU eligibility fixture: interior/boundary/gutter/coarse footprints, bounded
  and removed mappings, malformed bounds, mixed carrier IDs, overlapping
  triangles, nonplanar geometry and a rotated receiver.
- GPU integration fixture: actual residency/compositor/module sampling, a
  forced-full control, PBR/AUX/POM parity, material allocation accounting,
  mandatory tails, failed finite replacement, narrowing/removal and release.
- Existing whole-brick/shared-module JS checks cover 1×/2×/4× layouts, both wall
  sides, physical periods, negative phase and preserved finite ends.

The first CPU run (`coverage-v1`) exposed an incorrect test expectation: the
coarser page extends beyond the four-metre fixture once its stored gutter is
included. It must keep its full material. Preserve this failed run as evidence;
corrected assertion passes in `coverage-v2`.

All four `coverage-v2` MSVC targets built with **994 frozen source inputs**.
The CPU residency suite, receiver-material GPU integration, compositor,
direct-source raster/RT, surface parallax, offline VT export and connected
composed seams pass. GPU checks
report zero Vulkan validation errors, without RT skips or source changes.
The new receiver integration records nine exact material/AUX comparisons against
the full-page control, four post-write producer refusals, narrowing/removal and
reader retirement. It also directly exercises the complete-tail safety fallback
and proves that filling the eligible fine page adds no material allocation.
The seam regression retains both handed bends, 90-degree transitions, diagonal
charts and boundary rejection. These existing finite-material fixtures do not
establish mapped-module corner/curve acceptance.

Exact identities are recorded in [the final audit](final-audit.json),
[build manifest](../2026-09-16-shared-vt-pixels/coverage-v2-build-manifest.json)
and [test manifest](../2026-09-16-shared-vt-pixels/coverage-v2-tests.json).

These offscreen functional checks ran alongside the user's separate frozen r2
editor. Their duration is not an isolated throughput, latency or frame-time result.
The loader reports pre-existing missing/duplicate Epic overlay manifests; these
are separate from the zero-error Vulkan validation count.

## Remaining acceptance

Real wall captures and isolated timing remain pending. The frozen r2 editor is
a separate runtime and has not been replaced by this development build.

**Historical limitation of `coverage-v2`:** its fixture deliberately uses
the established no-enrichment VT test budget. The normal RT editor installs the
AO enricher with a default budget of two pages/frame; the current conservative
gate then keeps full receiver material pages. Consequently this checkpoint does
not establish avoided work in that default editor configuration. At that checkpoint,
mapped shading reads module ORM, so receiver-only AO writes are not a substitute
for an actual composed override. Preserve AO quality while adding receiver AO/
layer overrides, or prove geometrically that a particular receiver's enrichment
is an identity operation. Do not disable AO globally to make a benchmark pass.

The [subsequent receiver AO checkpoint](../2026-09-16-receiver-occlusion/README.md)
implements separate R16 factor pages and verifies coverage-only generation at
the normal two-page/frame enrichment budget, without private base copies. Its
own source/binary identities and added memory cost are recorded separately.

Sparse instance weathering, connected corner/cap/curve module acceptance and the
terrain/contact-blending workflow remain required. Weathered walls currently
retain their full finite material recipe.
