# Finite object evaluation adapter

`tools/object_eval.py` is a version 1, native MSVC capture and audited-mask
scorer for a single-root production world. It does not call a model. The rock
fixture and four-view rig are in `object-evaluation-rock-v1.json`. All paths in
`source_paths` are part of the frozen source closure; `model_write_paths` is
restricted to generator, scene-local material and preset files. Rig, references,
scorer and the production world remain outside that set.
The fixture pins `seed:2` in its world source, overriding Rock's default seed;
the manifest's `expected_root_params` must match native root provenance after
every fresh-process reload.
`freeze` checks JavaScript syntax with Node before creating the bundle; the
native reload job remains the authoritative bake check. The frozen bundle is
append-only by convention and checked against live source bytes before capture.

Run from the repository root on WSL with the checkout and output under `/mnt/<drive>/`:

```bash
python3 tools/object_eval.py freeze docs/agent/object-evaluation-rock-v1.json /mnt/d/tmp/rock-candidate-01
python3 tools/object_eval.py capture /mnt/d/tmp/rock-candidate-01 /mnt/d/tmp/rock-run-01
```

Each run directory must be new. `capture.json` is written even on a failed
launch or capture. A complete receipt includes exact candidate/rig/editor hashes,
the typed reload job, one presented-frame `viewport.capture` receipt per view,
PNG and numeric-channel hashes, camera/projection checks, captured-frame visible
detail readiness, and bake/settle/capture timings. Each view uses the typed
`view.set_camera` call, resets temporal history, waits the rig's presented-frame
count, then captures with `export_channels:true` and the manifest's
`desired_max_lod` (default 0). Any error
leaves `status: failed`. The adapter checks the native PNG and its `captured`
sidecar and rejects changed source bytes, an incomplete view set, failed bake,
transport failure, a missing production frame, or readiness blockers. The
`provisional_no_live_visible_set_barrier` receipt label is obsolete: the native
capture now supplies synchronized visible VG/VT/BLAS readiness.

## Candidate masks are derived and audited at capture

After a view's capture passes readiness, the adapter derives that view's
candidate mask from the capture's own `MECAP001` identity plane — never from
rendered color — and writes `<view>.mask.png` beside the image as a 0/255
8-bit grayscale PNG at capture dimensions (1280×720 for the rock rig). A pixel
is object coverage when it is not `UINT32_MAX` in both identity lanes, so
background and sky stay at 0.

`capture.json` records the verdict per view under
`views.<name>.mask_audited`, with `path`, `sha256`, mask and channel
dimensions, `object_pixels`, `coverage_fraction`, the bounding `bounds`, and one
boolean per criterion:

- `nonempty` — the mask has object pixels;
- `inside_frame` — the recorded bounding box of those pixels lies inside the
  frame;
- `matches_channel_dimensions` — the mask raster grid is the channel bundle's
  own grid, so no resampling happened between them;
- `area_within_bounds` — coverage is at most `MAX_MASK_COVERAGE` (0.9); a
  silhouette covering more of the frame than that is a coverage failure.

`audited` is the conjunction, `failures` lists the criteria that failed. A
failed audit fails the capture (the mask and its record are still written, so
the evidence survives in `capture.json`) — the scorer refuses to compare a
silhouette it cannot vouch for. If the internal raster extent differs from the
display PNG because scaling is active, the mask is resampled onto the display
grid, `resampled_to_image` is set, `matches_channel_dimensions` is false and
the capture fails closed: capture at a scale where those grids agree before
scoring.

## Building a reference from a baseline run

`reference-from-run` turns a completed baseline run into a version 1 reference
without hand-writing JSON or authoring a mask. It takes the run's own
`rig_sha256` and, per view, that view's captured image and its audited mask:

```bash
python3 tools/object_eval.py reference-from-run /mnt/d/tmp/rock-run-01 /mnt/d/tmp/rock-reference-v1.json
```

The four rig views of the rock fixture become the four reference entries. This
is a self-reference: scoring that same run against it must return IoU 1.0 for
every view, which is what makes it a usable baseline. Without `--output` the
reference is written into the run directory. With `--output` elsewhere the
pinned PNGs are copied beside the JSON, because the scorer resolves reference
entries relative to the reference file. An incomplete run, an unaudited or
changed mask, a changed image or a missing rig hash is refused, and an existing
reference is never rewritten. References should still be calibrated
same-specimen images, not different family specimens.

For scoring, a reference may equally be authored by hand, with the exact
`rig_sha256` from `candidate.json` and one entry per rig view:

```json
{
  "version": 1,
  "rig_sha256": "<64 hexadecimal characters>",
  "views": {
    "front": {
      "image": "front-reference.png",
      "image_sha256": "<SHA-256>",
      "reference_mask": "front-mask.png",
      "reference_mask_sha256": "<SHA-256>",
      "candidate_mask": "front.mask.png"
    }
  }
}
```

The same entry shape is required for every requested view, and every entry must
be a plain PNG filename in its own directory. Masks are audited 8-bit grayscale
binary 0/255 PNGs at capture dimensions and must be nonempty. `candidate_mask`
must name this run's audited `<view>.mask.png`: the scorer verifies the receipt
recorded that exact audit and hash, and never scores a hand-supplied or stale
mask. Reference images and reference masks live beside the reference JSON;
candidate masks live in the run directory. Scoring never infers masks from
rendered color and never returns a quality score for a failed capture. It
reports per-view silhouette IoU, symmetric contour distance normalized by image
diagonal, worst and mean IoU, and color difference over agreed foreground
pixels as a diagnostic. It is a provisional scorer; no image-quality acceptance
threshold is encoded.

```bash
python3 tools/object_eval.py score /mnt/d/tmp/rock-run-01 /mnt/d/tmp/rock-reference-v1.json
python3 tools/object_eval.py noise /mnt/d/tmp/rock-run-{01,02,03,04,05}/score.json
```

`noise` requires at least five complete unchanged candidate/rig/reference
scores from distinct runs and reports the 95th percentile paired silhouette
loss difference, the separate color diagnostic difference, and a provisional
minimum silhouette gain of at least 0.01 normalized loss. Include a cold
process in those repeats. Reopen a process after any persisted source change
and freeze a new bundle; session-only `procedural.update` values are never a
candidate. Retain each full run directory; neither the bundle
nor the source closure may be edited after freeze.
