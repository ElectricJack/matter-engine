# DLSS build and remaining castle optimizations

Follow-up: [output-aware material detail, adaptive shadows and new pass timers](lighting-quality-and-adaptive-shadows-2026-09-12.md)
are now implemented. This page preserves the original DLSS build comparison;
the follow-up records the newer executable and measurements.

The native MSVC editor now has an optional, working DLSS Super Resolution
build. The existing Streamline renderer integration was present, but the CMake
product targets forced `MATTER_HAVE_STREAMLINE=0`. This change connects an
explicit SDK option to those targets, stages the production runtime, and makes
distribution manifests and validation understand that optional dependency.

## Built artifact and reproduction

Development executable:
`D:/tmp/matter-castle-assembly/MatterEditor/build/windows-msvc-dlss/editor.exe`.
This is the current dirty castle integration worktree, not a clean release
package. The normal non-DLSS executable has a separate output directory.

From the repository root in native PowerShell:

```powershell
.\tools\build-windows.ps1 -Config RelWithDebInfo -Target matter_editor `
  -EnableStreamline -StreamlineRoot D:/SDKs/streamline-sdk-v2.12.0 `
  -EnablePhysx -PhysxRoot D:/PhysX-5.6.1 `
  -CudaRoot 'C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8'
```

Direct CMake configuration uses `MATTER_ENABLE_STREAMLINE=ON` and
`MATTER_STREAMLINE_ROOT`. The PowerShell wrapper explicitly switches the
feature off when `-EnableStreamline` is omitted. SDK files are external inputs.

The build stages `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll` and
`nvngx_dlss.dll`, plus both NVIDIA licenses. This is Super Resolution;
Frame Generation and Ray Reconstruction are not enabled. The required core
and feature plugin layout follows NVIDIA's
[Streamline integration guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md).
Installed SDK version: 2.12.0. Signature checks remain enabled.

Distribution staging records `features.streamline`, runtime hashes and notices.
Its checker permits the shared VC++ runtime required by these exact NVIDIA
DLLs, resolving it from Windows System32. The editor and other binaries retain
the static CRT policy. The current machine has the required VC++ runtime;
another machine needs it installed. The existing clean-source policy for
release packages remains in force.

## Matched visible measurements

RTX 4090, NVIDIA driver 610.74, same executable SHA-256
`bc2a0dbc8d2ede7c08b5776fb318d04edd6418cb970192fada231c170ad22c40`.
CastleUpgraded hall, 1920×1080 output, RT enabled, diffuse GI scale 0.128,
diffuse strength 1, reflection/refraction scale 1, four primary shadow samples,
secondary light sampling enabled, POM enabled. Fixed camera and exposure.
VSync off / MAILBOX; editor UI hidden but the real window visible and never
minimized in both measurements. Both windows were unfocused. Each run waited
for stable geometry, warmed for 15 seconds, then sampled for 20 seconds.

| Metric | Native | DLSS Quality |
|---|---:|---:|
| Internal resolution | 1920×1080 | 1280×720 |
| Median frame time | 15.488 ms | 8.537 ms |
| Median FPS | 64.57 | 117.13 |
| P95 frame time | 31.355 ms | 18.786 ms |
| Raw GPU total median | 14.935 ms | 8.248 ms |
| Raw primary local direct median | 7.319 ms | 3.440 ms |
| Raw combined GI median | 2.649 ms | 1.229 ms |
| Raw denoise median | 1.142 ms | 0.581 ms |
| Raw DLSS median | 0 ms | 0.386 ms |

This pair gives **81.4% higher median FPS / 44.9% lower median frame time**.
It is one static-camera pair, not a whole-castle or motion performance guarantee.
The primary-lighting pass remains the largest measured cost in both modes.
Reflection scale 1 means full **internal** resolution; DLSS reconstructs the
complete internally rendered image to output resolution.

Evidence: `C:/tmp/castle-dlss/perf-native-1080/` and
`C:/tmp/castle-dlss/perf-quality-1080/`. Each contains the exact settings,
executable hash, visibility observations, log, screenshot and performance JSON.
The benchmark helper now verifies selected **and active** DLSS mode and actual
output dimensions; silent Native fallback fails the measurement.

An additional matched Native run changed only primary shadow samples from four
to one: **10.281 ms / 97.26 FPS**, with raw primary local direct falling from
7.319 to **2.565 ms**. This is 50.6% higher median FPS than four samples in this
static view. Raw GPU total was 9.660 ms; P95 frame time was 22.486 ms. Evidence:
`C:/tmp/castle-dlss/perf-native-one-shadow-1080/`. This confirms the new priority,
but the one-sample penumbra is visibly noisier. Continuous motion acceptance is
still owed, and four samples remain the default. DLSS plus one shadow sample
was not benchmarked, so these percentage improvements should not be added.

The raw `composite` timer is the final swapchain display transform. It does
not measure the HDR lighting draw in `composite.frag`. Add that missing zone
before making claims about reconstruction cost; do not infer it by subtracting
independent per-pass medians.

## Quality and acceptance

Visible gold/glass captures exercised Native → Quality → Balanced →
Performance → Native, a small camera move and history settling. All seven
screenshots completed, the application exited normally, and the validation
layer reported no VUID/validation errors. Existing machine-level Vulkan loader
messages about stale Epic overlay manifests remain separate from those checks.
This run had an actual 1652×820 scene viewport despite requesting a 1920×1080
window; it is a functional/quality check, not another 1080p benchmark.

Gold highlights and glass reflections remain visible, and DLSS reduces edge
aliasing. **Fine brick relief is also softer.** The inspected primary material
path derives POM and final-channel mip levels from internal-resolution
derivatives (`gbuffer.frag:1109–1114`, `surface_detail.glsl:49–50`). Rendering
720p instead of 1080p increases that footprint 1.5×, or about +0.585 mip, before
DLSS sees the image. This is a concrete likely contributor, not a measured
attribution of all the softness to one cause. Native's harsher fine contrast
also includes aliasing.

The next DLSS quality experiment should compensate finished-surface sampling
for output resolution, starting with a −0.585 mip bias at this ratio and
applying it consistently to height, normals and material channels. Compare
relief against camera-motion shimmer before adoption. The audit found no
obvious jitter sign/normalization mismatch or extra full-frame TAA resolve.
POM-displaced depth with proxy-vertex motion remains a separate motion-quality
limitation. No speculative jitter or sharpening changes were made in this build.

Acceptance evidence is in `C:/tmp/castle-dlss/validation/`; comparison images
are linked from `C:/tmp/castle-dlss/comparison.html`.
Native Python package staging tests passed **21/21**, native PowerShell package
fixtures **33/33**, and the installed SDK's four staged DLL hashes matched.
These package tests use CPU fixtures, not hidden editor instances. The native
DLSS product build and repository whitespace check also passed.

The castle was left open in the DLSS build with Quality selected, GI scale
0.128, reflection/glass scale 1 and the editor controls visible. Live launch
receipt and screenshot: `C:/tmp/castle-dlss/live/`. With the docked panels open,
the live render viewport is smaller than the benchmark; the 1080p FPS figures
above belong to the controlled runs, not this editor layout.

## What to tackle next

1. **Reduce primary shadow work.** The existing one-sample option now has a
   measured static-view benefit. Add adaptive samples where temporal
   confidence is low. Four samples are still the default. A moving-camera,
   doorway and penumbra quality check is required before reducing it globally.
2. **Make DLSS retain more authored detail.** Run the output-aware material
   footprint experiment above. This is a quality refinement that can make the
   measured DLSS speedup more useful; it is not itself a promised FPS gain.
3. **Cull primary lights in screen/depth clusters.** World-space indexing and
   secondary stochastic sampling already exist. Primary direct and transmission
   still evaluate every contributing light. Keep world lists for off-screen RT
   hits, and measure candidate evaluations separately from shadow rays. Primary
   importance sampling/reuse is a larger follow-up if exhaustive visibility
   remains expensive.
4. **Cache static local shadows.** The castle has many stationary lights and
   occluders. Prototype a bounded cache with revision invalidation and dynamic
   occluder fallback. Preserve transparent visibility. This directly targets
   the dominant pass; a complete diffuse probe/surface cache is now lower priority.
5. **Specialize and schedule smaller work.** First add truthful HDR composite
   and per-GI-lane timers. Then test separate compiled raygens, cheaper materials
   for diffuse/rough secondary hits, selected-hit transmission shading, and
   selective denoising. Fixed 5/3/3 spatial filters and shared masked raygens
   remain. Avoid reintroducing material-zero holes in the low-resolution
   incident-light field. Adaptive POM previously failed to improve the hall,
   so treat shader fetch changes as experiments until measured.
6. **Continue startup optimization separately.** Compact runtime-ready part
   bundles, less repeated decode/validation and lower publication/upload cost
   remain useful. Subsecond startup has not been achieved. The earlier
   instrumented startup summed pipeline-creation calls to roughly 50 ms; it
   does not support blaming shader pipeline creation for multi-second stalls.
   See the [startup profile](castle-full-startup-profile-2026-09-11.md).

The [original roadmap audit](rt-lighting-roadmap-status-2026-09-12.md) is updated
to distinguish already completed independent GI/reflection rates and guided
reconstruction from the remaining work. Its old “GI is 81%” diagnosis applies
to the historical full-GI run, not this preferred quality configuration.
