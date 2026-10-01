# Native periodic material production

Follow-up: [independent module residency](../2026-09-16-periodic-material-residency/README.md)
now verifies shared ownership, activation and GPU retirement with real produced
pages. The remainder records the producer checkpoint and its integration limits.

Status: native page producer implemented; focused and regression checks pass. Receiver
registration/bindings, module lifetime/demand, connected POM integration and
sparse weathering remain open. Existing wall scenes still compose receiver-space
textures. This checkpoint does not establish a frame-time or scene-memory gain.

## Implemented

`vt_periodic_material.h` builds a generic immutable producer from a physical
repeat domain, a local direct-source base program and finite projected sources.
Its complete identity includes the source payloads, placements, base program,
projection frame, physical period and texel dimensions. It has no drawn receiver
or instance-weathering dependency. Brick layout remains an authoring concern;
the native API accepts arbitrary compatible projected sources.

Sources crossing a repeat boundary receive translated references with enough
support for finite filtering and normal derivatives through the coarsest mip.
These references share the original immutable payloads. Owned asynchronous
preparation, the existing finite-source spatial index, coherent material
composition and native BC7/BC5/R16 encoding produce the pages. The preparation
quad is never published as receiver geometry. Scene-dependent enrichment and
the scalar stub filler exclude independent module pages.

Each stored texel, including page gutters and partial final pages, resolves to
the same integer sample centre in the module. The physical footprint follows
each mip's actual width and height. Gutter subtraction uses nonnegative modular
arithmetic: GLSL leaves integer remainder with negative operands undefined.
See the [GLSL expression rules](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html#expressions).

## Compression and physical dimensions

The first real-clay run exposed a second boundary issue after height wrapping
was corrected: odd repeat dimensions put equal samples in different four-texel
BC blocks. In the 1045×193 case, the maximum normalized decoded differences were
0.078431 for color, 0.090196 for encoded normal components and 0.054902 for ORM,
despite zero R16 height mismatches. A wrapping coordinate alone cannot guarantee
identical compression at these different phases.

The producer now rounds requested density upward so every supported mip repeats
at the same BC block phase. A mip dimension must be divisible by four, or be one
or two texels. Thin axes reach the small cases without unnecessary padding.
Physical brick sizes and repeat distances remain exact. Explicit incompatible
producer domains are rejected; the independent sampler still supports odd-sized
domains as documented in its separate evidence.

For the 2.04×0.376-m brick module requested at 512 texels/m, the resulting logical
texture is 1152×256. Its complete mip chain requires the same 30 pages as the
initial 1045×193 domain. This fixture uses more of the available edge-page
space; other dimensions can require additional pages and must be measured.
Neither the domain nor the physical brick layout is forced to a square or a
power-of-two width.

## Validation fixtures

The analytic fixture places two constant sources, one across the repeat cut,
over recessed mortar. Its independent rectangle-coverage oracle checks physical
height filtering through source mips. A 259/128×131/128-m period requested at
128 texels/m yields a 288×160 compressed module.

The real-clay fixture places 32 bricks in eight columns and four running-bond
courses, sharing eight previously geometry-baked source payloads. The last brick
of each offset course crosses the repeat cut. Native test-only placement code
exercises the generic producer; it does not add a brick-specific DSL primitive.

Both fixtures inspect every 136×136 stored page at every supported mip, compare
repeated samples including gutters/padding, require exact equality of decoded
color, normal, ORM and R16 height, and check complete logical coverage. They
also require one preparation build across all pages, reject invalid replacement
inputs without losing the previous snapshot, and keep receiver geometry private.
The fixture recycles one readback slot while checking each emitted page; it does
not register 30 simultaneously resident pages or demonstrate scene ownership.

### Focused results (`producer-v5`)

| Fixture | Logical texels | Page fills | Repeated samples | R16 mismatches | Maximum decoded BC channel difference |
| --- | --- | --- | --- | --- | --- |
| Analytic rectangles | 288×160 | 10 | 123,760 | 0 | 0 |
| Geometry-baked clay | 1152×256 | 30 | 161,760 | 0 | 0 |

All **285,520** repeated-sample comparisons agree exactly. The analytic physical
height oracle's maximum error is **0.000000609 m**. The clay fixture references
eight payloads through 84 translated bindings, covering the 32 authored placements
and their finite filter support. Both fixtures reuse one preparation build.
The full compositor executable passes with zero Vulkan validation errors and
unchanged source, executable and brick-input hashes.

All five MSVC targets build, including the editor, and all ten native regression
checks pass: chart atlas, CPU residency, independent material sampling, compositor,
queue lifetime, composed seams, composed parallax, input snapshots, direct sources
and surface parallax. Vulkan checks report zero validation errors with no RT skips.
The final audit confirms all 920 tracked source hashes and five executable hashes
match their builds and every test ran the corresponding built executable. These
are correctness results, not frame-time or edit-latency measurements.

Artifacts:

- [Native clay run and input/output hashes](producer-v5-clay.json)
- [Complete compositor log](producer-v5-clay.log)
- [Decoded material channels and repeat preview](producer-v5-channels.png)
- [MSVC build manifest](../2026-09-16-shared-vt-pixels/producer-v5-build-manifest.json)
- [Regression results](../2026-09-16-shared-vt-pixels/producer-v5-tests.json)
- [Final source/binary/evidence audit](producer-v5-final-audit.json)

The preview was visually inspected: the wrapped running bond, geometry-derived
dents/chips and recessed mortar are present in the normal/height channels.
It deliberately has no POM, lighting, instance weathering or terrain blending;
it is not visual approval of the final wall workflow.

## Reproduction and limits

Build and native test helpers in the sibling shared-page evidence directory bind
results to exact source/executable hashes. `run_clay.py PREFIX` also hashes all
eight input `.fst` files and the raw decoded channel outputs. `plot_channels.py
PREFIX` converts successful native output to a labeled material-channel image;
that image is not a rendered wall or a POM/lighting comparison.

The failed `producer-v1` through `producer-v4` logs remain available. The first
run omitted asynchronous preparation in the fixture; the second exposed gutter
arithmetic; the third isolated compression phase differences after height passed.
The fourth passed real-clay channel equality and exposed dimension rounding
that can add a mip; the factory now repeats alignment when that happens.
Acceptance thresholds were tightened to exact repeated-channel equality rather
than relaxed to absorb those differences.

Next: register and retain a module independently, publish receiver mappings only
after its tail is active, request module pages from receiver/POM demand, and carry
explicit normal-frame/datum conversion. Demonstrate 1×/2×/4× shared wall rendering
and avoided base generation, then add finite ends/corners/curves and sparse
weathering under the [material-domain plan](../../../superpowers/plans/2026-09-16-periodic-material-domains.md).
Terrain/contact blending and full visual/performance acceptance remain required
by the broader goal.
