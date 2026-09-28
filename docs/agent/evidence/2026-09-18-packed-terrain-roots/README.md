# Packed exact terrain roots

## Representation change

Optional `CompileConfig::packed_root_triangles` combines nearby terminal
roots into bounded pages. Eligibility requires no children and zero geometric
error. Each source triangle and all its corner attributes are copied exactly;
no simplification or interpolation crosses a material/UV/normal/AO seam.
Morton ordering of island centres provides deterministic spatial grouping.
Absorbed leaf nodes are removed, surviving child indices are remapped, and
refining roots keep their existing hierarchies. Output triangle budgets are
unchanged by the packing pass.

Terrain enables a 512-triangle cap. The compiler staging cap remains enforced.
Geometry keys use `terrain-charted-geometry-v3-packed512-z1`; prepared-sector
policy uses `terrain-v3-packed512-z1/prepared-2`, forcing regeneration of both
records. Other asset types retain their existing compiler policy.

This addresses the large number of tiny mandatory root pages. It does not
pretend to remove attribute seams for simplification, nor does it complete
sector-wide bank allocation or VT disk-cache preparation.

## Validation

Native geometry hierarchy tests pass. New checks pack 128 single-triangle
material islands into four pages with no dead leaves; verify exact triangle
coverage, winding, positions, material identity, tint, UVs, normals, AO and
receiver coordinates; enforce page size; retain a connected refining
hierarchy; validate mixed packed/refining child-index remapping; and verify
byte-identical encoded pages across repeated compilation. Existing seam, cut, paging, retry and prepared-sector tests pass.

The full-load audit now also requires zero rejected/unready geometry assets,
zero source fallback instances for geometry-enabled sources, and an active VT
system with zero rejected variants and zero queued fills. It polls VT stats
while retaining the existing stable-region/quiet-period guard. This prevents
an empty upload queue with coarse fallback or pending textures from being
reported as complete.

## Runtime results

Cold preparation completed: all 2441 sectors and 815 new geometry assets were
written successfully. Sector hashes match the original region. Preparation
reached streamer idle in 1130.27 seconds; native test builds overlapped this
run, so it is not a controlled baking performance comparison.

The rebuilt editor reopens all 2441 prepared sectors and all 815 geometry keys
without generation, compilation or cache misses. Reopen streamer-idle time is
16.30 seconds, engine sector-fill time 12.94 seconds. Packing has not materially
changed this admission time (previously 15.98 seconds to streamer idle).

Full VG with 4 MiB read-ahead now admits **all 775 active geometry assets**, with
**zero rejected/unready assets and zero source fallback**. Page entries are
roughly 27000 instead of saturating the 262144 ceiling. VT fills finish with
zero queue/rejected variants. However, fine-page loading did not finish within
180 seconds: the CPU page bank reached 1 GiB, with repeated budget deferrals
and evictions. Final geometry-update window averaged 7.50 ms, and the final
single-frame snapshot was 34.54 ms CPU / 6.53 ms GPU. These are streaming
measurements, not proof of a stable <10 ms total frame time.

The same-scene 0 MiB read-ahead experiment avoids CPU deferrals initially but
loads over 73000 page entries and reaches the 1 GiB GPU limit, then thrashes.
It also fails complete readiness at its 120-second cutoff. Thus simply
turning off prefetch is insufficient. Both runs preserve exact cache-key sets
and report zero compilation/read failures. `valid: false` is intentional:
coverage from resident ancestors is not the same as completed requested detail.

Next investigation has two concrete sources: the sector coordinator waits
50 ms before each refill (`matter_engine.cpp` worker loop), while GPU geometry
refinement requests occur before frustum culling (`geometry_cut.glsl` and
`geometry_select.comp`). Bound visible refinement demand, then measure it
with unchanged scene/detail settings. Diagnose pinned read-ahead amplification
separately rather than hiding it behind a larger bank.

The <1 second / <10 ms objective remains open. Screenshots still show coarse
terrain/lighting artifacts; no visual-polish or seam-free claim is made.

## Reference-directory reuse

`PageCache::read_manifest` now retains its decoded reference table while the
atomic `refs.bin` file stamp is unchanged. The stamp is captured before opening
the table so a concurrent publication cannot label an old table with a new
stamp. Failed reloads return an error rather than silently serving the prior
reference. Missing files are rechecked. Payload/index validation remains in
the existing read path. A reload counter verifies reuse without relying on a
timing threshold. Prepared-sector lookup now uses this shared implementation
instead of maintaining a separate reference-directory reader.

The native AssetStore suite passes 617 checks, including repeated-hit reuse,
same-size atomic replacement, erase and republish through an existing reader.
This change is included in the editor built after preparation; the running
cold preparation still uses the earlier executable.
