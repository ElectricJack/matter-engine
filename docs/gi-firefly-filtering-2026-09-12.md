# Reduced-resolution GI firefly filtering

The diffuse temporal pass now rejects bright outliers before they enter its
radiance history or luminance moments. Previously, the pass clamped old history
but admitted a bright new sample; that sample also inflated variance and could
spread through the spatial filter and low-resolution reconstruction.

The implementation is in `MatterEngine3/shaders_vk/gi_temporal.comp`. It applies
only to diffuse GI when its extent differs from the G-buffer extent. Full-rate
diffuse, reflections, transmission, primary direct lighting, the push-constant
layout, and resource allocations retain their existing contracts. Filtering
operates on indirect illumination before full-resolution material detail is
applied.

## Filter policy

- Use center-excluded median/MAD luminance statistics from compatible 3×3
  neighbors. Require both identity words to match, normal agreement of at least
  0.85, and compatible depth. If fewer than three peers qualify, search the
  complete outer 5×5 ring. Edge texels are never duplicated.
- Limit luminance while preserving RGB proportions. The ceiling is
  `max(0.25, 4 * median, median + 6 * MAD)`, bounded by the existing half-float
  numeric range. This is a ceiling policy, not an added ambient-light floor.
- An unsupported first sample also gets the 0.25 ceiling. This closes the
  escape path responsible for remaining bright spots on narrow wooden parts
  and floor-tile edges. Even the first valid history can preserve established
  fill, but a clipped sample cannot repeatedly multiply itself into a much
  larger ceiling without confirmation.
- Existing diffuse auxiliary history stores a candidate luminance and a
  repetition confidence. Similar samples build confidence; it decays by 63/64
  per valid frame. Permission increases smoothly between confidence 3 and 4.
  Inconsistent flashes cannot combine their evidence. This lets persistent
  sparse or thin-surface lighting build up gradually without new textures.
- Compatible neighboring history supports established indirect fill. Old
  radiance and excessive variance are bounded; invalid samples, history, and
  reprojection inputs cannot poison half-float moments.
- Diffuse accumulation grows to 96 frames, with a minimum incoming weight of
  1/96 instead of 5%. Existing reactivity and geometry/reset rejection remain
  available. The previous temporal depth tolerance is preserved; the tighter
  depth test is confined to spatial support.

This deliberately favors stability over an unbiased Monte Carlo estimate.
Very rare or inconsistent indirect contributions are reduced, and uncertain
thin receivers light up gradually. Increasing the history length alone would
not fix first-frame flashes or variance poisoning. An earlier spatial-only
version also failed a sparse-light cold-start test; recurrence confirmation
addresses that failure without treating one isolated hit as sufficient evidence.

## Validation

Native MSVC editor and Vulkan smoke builds pass. The visible `gi-firefly` smoke
mode executes 789 production-shader dispatch probes, including real GPU history
sequences, with zero Vulkan validation errors. It covers isolated and repeated
samples, geometry boundaries, 5×5 fallback support, chroma, uniform bright
lighting, finite values, poisoned moments, reactivity, reset behavior, and the
unchanged full-rate/reflection/transmission/direct contracts.

A 200-unit isolated sample on an unsupported thin receiver peaks at 0.249878
and decays to 0.002218 after 108 frames. A sustained 4-unit signal on that same
receiver reaches 3.832 after 216 frames. A rotating one-in-nine 4-unit source
reaches 86.7% of its expected average after 216 frames, demonstrating gradual
recovery from a black history. A changing-magnitude flash sequence remains
unconfirmed. The existing visible `rt-local-direct` regression suite also
passes with zero validation errors.

The capture helper now rejects unknown property commands. An exploratory
reflection-isolation timeline attempted properties that the editor does not
expose; those segments are explicitly excluded in its artifact README. The
supported diffuse-strength switch, with reflections still enabled, identified
the residual wood/floor flashes as diffuse GI. The helper's 11 existing CPU
tests pass.

## Evidence

Artifacts are in `C:/tmp/castle-gi-fireflies/`. `baseline/` preserves the starting
editor and runtime dependencies; `baseline-source/` preserves the original
relevant files. `before/` and `after/` contain stationary hall/gold sequences,
a moving-camera sequence, and settled captures. `compare.html` plays matching
capture samples side by side; playback timing is illustrative, not measured
display cadence. Intermediate rejected variants remain available separately.

The last capture sequence contains camera movement during nominal stationary
segments, and its output size also differs from the baseline. It is retained
for visual inspection only; its temporal pixel statistics are invalid as a
noise comparison and are excluded. Earlier fixed-camera captures demonstrated
substantial filtering but also reduced mean brightness, so they must not be
presented as unbiased illumination preservation. The controlled GPU sequences
are the quantitative evidence for the final unsupported-sample fix.

The user requested an immediate interactive relaunch during the final performance
run. That run was stopped; no final GPU-overhead claim is made. The preserved
baseline denoise median is 0.584 ms at verified 1920×1080 output. The live editor
uses the latest build, DLSS Quality, RT/GI on, diffuse scale 0.128, full-rate
reflections, IMMEDIATE presentation and the existing 90 FPS limit. No further
automated graphics runs will interfere with that interactive session.
