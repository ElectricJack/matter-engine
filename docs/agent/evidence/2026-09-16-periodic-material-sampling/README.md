# Independent periodic material sampling

Follow-up: [native periodic material production](../2026-09-16-periodic-material-producer/README.md)
now verifies compressed pages generated from real brick sources. The remainder
of this document records the earlier sampler checkpoint and its integration limits.

Status: native GPU sampler implemented; focused and regression checks pass. Runtime
module production, receiver bindings, demand/lifetime integration and sparse
weathering remain open. The brick proof scenes still compose receiver-space
textures; this checkpoint does not change their appearance or storage costs.

## Implemented

`vt_material_domain.glsl` resolves two complete addresses: receiver coverage and
geometry, and an independently addressed repeating material. A receiver page
can therefore sample several material pages. The material supplies all four
continuous channels and its own height decode/input snapshot; categorical AUX
and geometry stay with the receiver. Separate feedback requests are available.
Missing/invalid module records or a generation mismatch retain all channels of
the complete receiver fallback together.

Coordinates wrap before material page lookup. Derivatives remain unwrapped,
and lookup uses each mip's actual logical width/height. Finite receiver sampling
retains its existing clamped path.

### Non-power-of-two parent correction

A 259-texel module becomes 129 texels at mip 1. The boundary at coarse x=128
corresponds to fine x=256.99, rather than x=256. One fine page overlaps two
coarse pages, while the existing CPU page-table propagation uses integer page
ancestry. Using its indicated slot with a separately recomputed in-page position
can sample the wrong page.

Periodic lookup now re-resolves the actual coordinates at the indicated fallback
mip. If that corrected page is also missing, it advances again toward the pinned
tail. Work is bounded by the eight supported mips. The finite receiver path is
unchanged. This establishes correct fallback sampling; it does not redesign CPU
demand priority to select the finest available overlapping page.

## Focused native evidence

The MSVC `domain-v2` build includes the editor and four native test executables.
The `vt-material-domain` smoke mode executes the production sampler against:

- Three differently sized receivers, with distinct coverage, geometry tokens,
  fallback materials and immutable input snapshots.
- One 259×131 logical module: nine pages over three mips, with material pixels
  deliberately stored in another physical pool layer.
- Fully resident, tail-only, coarse-only and partially resident coarse tables.
  The partial tables come from the production `VtIndirectionMap` builder.
- Negative/exact-boundary coordinates, either side of repeat/page boundaries,
  broad footprints beyond the tail, stale generations, absent/out-of-range slots,
  invalid input coordinates/derivatives and absent receiver coverage.

**741 probes pass**, with zero Vulkan validation errors. All four material
channels agree with a dense periodic image sampled independently of the page
table: maximum normalized channel error **0.000023872**. Height-decode error is
**0.000000002 m** in this synthetic fixture. Receiver identities and both feedback
requests are checked separately. A control using the original single lookup
produces **12 wrong-parent samples**, while the corrected path has zero failures.

This is a deterministic RGBA8 addressing/filtering fixture with synthetic wrapped
gutters at every mip. It does not validate the future brick module producer's
compressed mip generation, normal-frame conversion, finite end treatments or
connected POM module binding. It is not a timing, memory-saving or visual-approval
result. The earlier `domain-v1` checkpoint passed 309 probes before the partial
residency cases and their correction were added.

All five MSVC build targets and all ten native checks pass: material-domain,
chart atlas, CPU residency, compositor, queue lifetime, composed seams, composed
parallax, input snapshots, direct sources and surface parallax. Vulkan checks
report zero validation errors, with no RT skips. Source and executable hashes
remain unchanged through the runs. Existing seam fixtures retain their previous
non-flattened results, including orthogonal and diagonal chart crossings.

Artifacts:

- [Build manifest](../2026-09-16-shared-vt-pixels/domain-v2-build-manifest.json)
- [Source hashes](../2026-09-16-shared-vt-pixels/domain-v2-sources.json)
- [Native test results](../2026-09-16-shared-vt-pixels/domain-v2-tests.json)
- [GPU sampler log](../2026-09-16-shared-vt-pixels/domain-v2-test-vt-material-domain.log)

## Next integration step

Create real canonical module pages independently of receiver meshes, with wrapped
filter support and per-axis physical texel metrics. Register/cache them once per
complete immutable source identity. Publish chart mappings only after the module
tail is active; retain its table, inputs and geometry dependencies through all
GPU readers. The 32-bit generation check detects stale bindings but does not
provide ownership or authorize publication before tail readiness.

Map each chart's physical frame to module UV and derivatives; carry normal-frame
conversion and displacement datum explicitly. Feed module requests from visible
receiver demand and POM travel. Adapt ordinary and connected POM to validate
receiver coverage while reading module material/height. Then demonstrate native
1×/2×/4× brick reuse and avoided base generation before adding sparse overrides.
Continue the full [material-domain plan](../../../superpowers/plans/2026-09-16-periodic-material-domains.md),
including ends/corners/curves and the broader terrain/contact-blending goals.
