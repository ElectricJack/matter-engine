# Castle frame pacing

The largest repeatable source of jitter in the tested castle view was swapchain
acquisition. With MAILBOX presentation, about every fourth frame waited another
9–11 ms for an image, followed by a very short frame. Explicit IMMEDIATE
presentation removes that pattern on this machine. Adding a 90 FPS CPU limiter
makes the application cadence substantially more consistent while retaining
the same RT, GI, DLSS and material settings.

This Windows session is Remote Desktop, reporting **1728×1084 at 32 Hz** through
`EnumDisplaySettingsW` and `SM_REMOTESESSION`. That is a material limitation of
the measurement environment. CPU rendering/submission cadence and remote display
cadence are different quantities. We have demonstrated the former improvement;
we have not measured a 90 Hz displayed image stream. The global presentation
default remains unchanged, and the new limiter defaults to unlimited.

## Evidence

RTX 4090, NVIDIA 610.74, CastleUpgraded hall, visible foreground windows,
1920×1080 output. DLSS Quality uses 1280×720 internally, diffuse GI scale 0.128,
reflection scale 1, four primary area samples, primary light culling, secondary
light sampling, POM and output-aware material sampling. Each run has 15 seconds
of warmup followed by 20 seconds of sampling. No concurrent builds or graphics
tests. Validation and extra GPU lighting timers are disabled for these numbers.

The following are **successive CPU post-present intervals**, including work
between iterations. The older `median_frame_ms` measurement excludes some
post-present bookkeeping and its reciprocal can overstate sustained throughput
when frames arrive in bursts. Mean FPS below is 1000 / mean interval.

| Quality configuration | Mean FPS | Median ms | p95 ms | p99 ms | Standard deviation ms | Maximum ms |
|---|---:|---:|---:|---:|---:|---:|
| MAILBOX, unlimited | 105.17 | 8.812 | 19.617 | 20.339 | 6.254 | 21.590 |
| IMMEDIATE, unlimited | 120.30 | 7.943 | 11.076 | 13.216 | 1.510 | 17.135 |
| IMMEDIATE, limit 90 | 89.20 | 11.195 | 11.793 | 12.123 | 0.375 | 14.934 |

The capped result reduces interval standard deviation by **94%** against the
MAILBOX baseline and p95 by **40%**. This is a smoother cadence at a deliberately
lower rendering rate, rather than a rendering-quality reduction. The target is
an upper limit; timer wake-up and scheduling overhead make the actual average
slightly lower. A cap cannot force a frame that exceeds its budget to finish.

All three receipts use executable SHA-256
`75b3908cf6dcc05b84c4444050f3b3044956a35d599c80ed520d9076fb36d824`,
with no publication uploads, immediate submissions or DLSS resets during
sampling. They contain 2105, 2406 and 1784 frames, respectively. Visibility
polls confirm foreground, visible, unminimized windows. The independently
measured uncapped presentation comparison also reduced loop p95 from
19.650 ms MAILBOX to 11.395 ms IMMEDIATE.

Evidence directory: `C:/tmp/castle-frame-pacing/`. The main receipts are
`acceptance-quality-mailbox/`, `acceptance-quality-immediate/` and
`acceptance-quality-90/`. `analysis.json` contains raw-span summaries;
`frame-pacing.png` and `frame-pacing.svg` plot consecutive intervals and their
distributions. `baseline/` and `baseline-source/` preserve the starting binary
and relevant sources. `final-source/` preserves the implemented files.

## What the trace says

The initial diagnostic run measured a 9.251 ms p95 frame-fence wait plus a
separate 10.367 ms p95 acquisition wait. Cleanup, the previous acquisition
fence, the prior-present fence and queue present were small. The raw GPU total
was 8.355 ms median / 10.558 ms p95 while CPU loop p95 was 19.631 ms. These
are distributions of different frames, so they must not simply be added or
subtracted as if they described one frame.

Individual slow rows establish the double wait: for example, serial 2032 spent
8.356 ms at the frame fence and 11.087 ms acquiring the image, producing a
21.043 ms loop. Other rows show roughly 1 ms loops immediately after a stall.
In the final IMMEDIATE cases, acquisition p95 is about 0.01 ms. No fence was
removed and no synchronization contract was weakened.

FIFO naturally followed the remote display, measuring a 31.264 ms median loop
and 33.670 ms p95. MAILBOX limited to 30 FPS avoids acquisition spikes here:
33.458 ms median / 35.252 ms p95 CPU present interval, 29.88 FPS average.
That provides a measured option which retains MAILBOX presentation, although
it does not prove perfect alignment with remote scanout. IMMEDIATE can tear;
that is why it is an explicit choice rather than a replacement global default.
See the [Khronos presentation-mode contract](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_swapchain.html).

The Streamline bridge forwards acquisition to its interposer when active;
there is no local sleep or Reflex feature integration in that path. Selecting
Native rendering alone does not bypass the interposer. The remote presentation
path is a strong explanation for the observed backpressure, but the traces do
not isolate compositor internals from driver/interposer behavior.

## Implemented controls and diagnostics

- `MATTER_PRESENT_MODE=auto|fifo|mailbox|immediate`: strict explicit swapchain
  selection. Auto retains the existing VSync policy. Unsupported or malformed
  explicit values fail instead of silently changing the comparison. Creation
  logs requested/effective modes, supported modes, image count and extent.
- `render.gpu.frame_limit`: live, persisted user setting in Performance → GPU
  Features, 0–360 FPS, zero unlimited. `MATTER_FRAME_LIMIT` can force it at
  launch. The limiter runs before input sampling and uses actual wake times
  to avoid catch-up bursts after hitches. Long waits poll window events every
  25 ms; minimize/resume resets scheduling.
- Windows pacing uses an owned high-resolution waitable timer when available,
  portable sleep fallback, and a bounded 0.2 ms yield tail. It does not spin
  for a whole frame or change global timer resolution. The Windows flag is
  documented by [Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw).
- `MATTER_FRAME_TIMINGS=1`: optional CPU wait diagnostics. Perf mode buffers a
  bounded raw trace in memory and writes it after sampling, avoiding per-frame
  diagnostic disk writes. Serials and GPU readback sequences are retained;
  GPU times are explicitly identified as the latest retired sample.
- `present_cadence_statistics`: always available in perf results, with actual
  CPU interval mean/FPS, median, p95/p99, min/max and standard deviation. The
  existing metrics remain available for compatibility. Pacing has its own
  loop timing in the UI, perf output and issue reports.

The helper accepts these controls and explicitly forces the default benchmark
frame limit to zero, preventing saved UI preferences from silently capping a
run. It can launch [PresentMon](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md)
for separate display evidence. PresentMon 2.5.1 started and stopped here but
created no CSV; its privilege warning and empty capture are preserved. Exit 0
is therefore not reported as successful display measurement.

## Validation and limits

Native MSVC editor and `frame_pacer_tests` builds pass. The tests cover disabled
pacing, normal deadlines, late frames, oversleep, rate changes, reset/resume,
rate clamps and the interruptible production wait. An Astra review checked
the scheduler and diagnostic time bases. A test helper named `near` initially
collided with a Windows macro; it was renamed before the passing native run.

Visible validation runs exercise MAILBOX and IMMEDIATE. The additional
`validate-controls/` capture switches 90 → 60 → unlimited → 90 through FIFO,
confirms each property value, moves between hall and glass/gold views, switches
RT to raster and back, and resizes the swapchain from 1280×720 to 960×540.
All four requested PNGs/sidecars landed and the editor exited normally.
No new Vulkan correctness error was observed. Existing unused shader-interface
warnings and stale external Epic overlay manifest loader messages remain.

The exploratory `final-quality-mailbox`, `final-quality-immediate` and
`final-quality-90` runs had validation enabled because this
editor interprets presence of `MATTER_VK_VALIDATION` as true, even for value
`0`. The helper now omits the variable when disabled. Those runs are retained
as functional evidence and excluded from the performance table above.

Native capped at 60 measured 16.770 ms median / 17.962 ms p95, but one frame
still reached 50.917 ms. That frame spent 27.317 ms in poll/input and 6.798 ms
in UI, with ordinary pacing and negligible acquisition. Another Native outlier
had a 9.902 ms CPU-render span. Such wall-time measurements cannot distinguish
expensive CPU work from OS descheduling. The new present intervals also confirm
that omitted post-present bookkeeping is normally only about 0.004 ms and is
not responsible for these hitches.

## Next priorities

1. Repeat display/latency measurements on a local high-refresh display, including
   VRR and a moving-camera path, before selecting a global presentation policy.
   Keep per-machine presentation and sustainable FPS choices explicit.
2. Capture OS scheduling/CPU call stacks around the remaining poll/UI/render
   outliers. Their costs are separate from the repeatable acquisition stalls
   fixed by the explicit presentation configuration. Do not guess at an
   expensive engine function from wall time alone.
3. If low-latency, tear-free MAILBOX still stalls locally, compare image-count
   policies and audited late acquisition/offscreen submission. A fourth image
   is plausible extra buffering, not a proven fix; it can also increase queued
   work. Keep all present-completion ownership guarantees.
4. Measure moving-view GPU budget overruns, pipeline creation, streaming uploads
   and publication work. Warm/cached pipelines and bounded per-frame background
   work address those stalls if traces show them. Static local-shadow caching
   remains a separate opportunity to make more FPS targets sustainable.

No geometry, shaders, RT sample counts, GI reconstruction quality, glass,
materials or global rendering-quality defaults changed in this work.
