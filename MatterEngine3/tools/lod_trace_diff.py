#!/usr/bin/env python3
"""lod_trace_diff.py A.trace B.trace — exit 0 iff two MATTER_LOD_TRACE runs agree.

The M1d fly-through determinism gate (docs/superpowers/plans/
2026-08-04-lod-vt-migration.md). Both traces come from the SAME machine running
the SAME scripted camera path twice, warm. No golden trace is committed, for the
reason docs/baselines/README.md gives for PNGs: float ties at a switch boundary
can land differently across GPU and compiler, so a checked-in expectation would
be a cross-machine claim the test cannot make.

Two clean warm runs come out byte-identical, so `cmp` would in fact suffice for
the comparison itself. What `cmp` cannot do is the part that keeps this gate
honest:

  * a trace that is empty, that never draws anything, that never crosses a
    switch distance, or that carries a `!` line (a zero instance_token, a
    duplicate key) FAILS here rather than matching. Without those checks the
    gate is satisfiable by rendering NOTHING — two runs that both draw an empty
    world are byte-equal — or by flying a path on which no rung ever moves,
    which is a visibility test wearing a LOD test's name;
  * a run killed before lod_trace::close() has no `S` summary line, and a
    truncated trace must not read as agreement;
  * --ignore-frames drops the trailing `#f<n>` stamp and compares only the ORDER
    of events. The stamp is the MATTER_CAM_PATH pose index, which is
    run-independent by construction (one pose per rendered frame) and so is
    compared by default; the renderer's own frame serial is never used, because
    it is NOT stable — two warm runs put the same first switch on serial 37 and
    36. Use --ignore-frames for a world whose streaming shifts poses anyway.

`E` events are already sorted by (instance_token, cluster_index) within a frame
by the emitter, because GPU transform slots come from an atomicAdd and their
order races between otherwise identical runs.
"""
import argparse
import sys


def load(path, keep_frames):
    """Returns (lines, census_max_pairs, summary, complaints)."""
    lines = []
    max_pairs = 0
    summary = None
    complaints = []
    version = None
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            body, _, stamp = raw.partition("#")
            text = body.strip()
            if not text:
                continue
            if keep_frames and stamp.strip():
                text = text + " @" + stamp.strip()
            kind = text.split(None, 1)[0]
            if kind == "V":
                version = text
                continue
            if kind == "!":
                complaints.append(text)
                lines.append(text)
                continue
            if kind == "C":
                parts = text.split()
                if len(parts) >= 2:
                    try:
                        max_pairs = max(max_pairs, int(parts[1]))
                    except ValueError:
                        complaints.append("malformed census: " + text)
            if kind == "S":
                summary = text
                continue
            lines.append(text)
    if version is None:
        complaints.append("no V version line; is this a lod trace?")
    if summary is None:
        complaints.append(
            "no S summary line; the run was killed before lod_trace::close()")
    return lines, max_pairs, summary, complaints


def count_events(lines):
    """Returns (all events, events where a rung actually MOVED).

    An `E ... - 0` or `E ... 3 -` is a visibility change: the cluster started or
    stopped being drawn. Only an event with a rung on BOTH sides is a switch.
    """
    events = 0
    rung_changes = 0
    for line in lines:
        if not line.startswith("E "):
            continue
        events += 1
        parts = line.split()
        if len(parts) >= 5 and parts[3] != "-" and parts[4] != "-":
            rung_changes += 1
    return events, rung_changes


def keys_of(lines):
    """The (instance_token, cluster_index) pairs an event stream mentions."""
    keys = set()
    for line in lines:
        if not line.startswith("E "):
            continue
        parts = line.split()
        if len(parts) >= 3:
            keys.add((parts[1], parts[2]))
    return keys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--min-pairs", type=int, default=1,
                    help="fail unless some frame drew at least this many "
                         "(instance, cluster) pairs; the anti-vacuity check")
    ap.add_argument("--min-rung-events", type=int, default=1,
                    help="fail unless the path moved a rung at least this many "
                         "times. 0 accepts a visibility-only trace")
    ap.add_argument("--ignore-frames", action="store_true",
                    help="compare only the ORDER of events, not the camera-path "
                         "pose each landed on. Needed for a world whose "
                         "streaming shifts poses between warm runs")
    args = ap.parse_args()

    keep_frames = not args.ignore_frames
    a_lines, a_pairs, a_summary, a_bad = load(args.a, keep_frames)
    b_lines, b_pairs, b_summary, b_bad = load(args.b, keep_frames)

    failed = False
    for path, complaints in ((args.a, a_bad), (args.b, b_bad)):
        for complaint in complaints:
            print("BAD  {}: {}".format(path, complaint))
            failed = True

    # Anti-vacuity, checked on BOTH sides before the comparison: a gate that
    # passes because nothing was drawn is worse than no gate.
    for path, pairs in ((args.a, a_pairs), (args.b, b_pairs)):
        if pairs < args.min_pairs:
            print("BAD  {}: peak drawn pairs {} < {} -- the world drew nothing, "
                  "so this run proves nothing".format(path, pairs,
                                                      args.min_pairs))
            failed = True
    events_a, rungs_a = count_events(a_lines)
    events_b, rungs_b = count_events(b_lines)
    for path, events in ((args.a, events_a), (args.b, events_b)):
        if events == 0:
            print("BAD  {}: no events at all -- nothing was ever drawn or "
                  "undrawn, so this run proves nothing".format(path))
            failed = True
    for path, rungs in ((args.a, rungs_a), (args.b, rungs_b)):
        if rungs < args.min_rung_events:
            print("BAD  {}: {} rung-change events < {} -- the camera path never "
                  "crossed a switch distance, so this is a visibility test, "
                  "not a LOD test".format(path, rungs, args.min_rung_events))
            failed = True

    # A specific diagnosis for a failure that otherwise reads as hundreds of
    # unrelated DIFF lines: the two runs did not disagree about SELECTION, they
    # disagreed about who the instances ARE.
    #
    # Both halves of this key were once allocation-ordered, and a streamed world
    # scored 5 of ~132 surviving pairs here. instance_token derived from
    # WorldSession's instance_id, which for a streamed sector was
    # `sector_next_id++`; and cluster_index was the renderer's global cluster
    # slot, i.e. PartRecord::cluster_start (part-registration order) plus a
    # local offset. Both are now content-derived — see
    # matter_engine.cpp::sector_instance_id and
    # FrameResources::lod_trace_local_cluster — and PomProofBrick scores 48/48.
    # This check stays as the regression guard for that: if it ever fires again,
    # some new identity has been keyed on an allocation counter.
    a_keys = keys_of(a_lines)
    b_keys = keys_of(b_lines)
    if a_keys and b_keys:
        overlap = len(a_keys & b_keys) / float(min(len(a_keys), len(b_keys)))
        if overlap < 0.5:
            print("BAD  the two runs share only {:.0%} of their (instance_token,"
                  " cluster) keys. This is not a selection difference -- the "
                  "instance IDENTITIES differ between runs, so some part of the "
                  "trace key has been derived from an allocation counter rather "
                  "than from content. Both known offenders were fixed on "
                  "2026-08-04; look for a THIRD.".format(overlap))
            failed = True

    if a_summary != b_summary:
        print("DIFF summary")
        print("  A: {}".format(a_summary))
        print("  B: {}".format(b_summary))
        failed = True

    if len(a_lines) != len(b_lines):
        print("DIFF record count {} vs {}".format(len(a_lines), len(b_lines)))
        failed = True
    shown = 0
    for index, (left, right) in enumerate(zip(a_lines, b_lines)):
        if left == right:
            continue
        failed = True
        if shown < 20:
            print("DIFF record {}\n  A: {}\n  B: {}".format(index, left, right))
            shown += 1
    if shown == 20:
        print("... further differences suppressed")

    if failed:
        print("FAIL {} vs {}".format(args.a, args.b))
        sys.exit(1)
    print("MATCH {} records, {} events ({} rung changes), peak {} drawn pairs "
          "({} vs {})".format(len(a_lines), events_a, rungs_a, a_pairs, args.a,
                              args.b))
    sys.exit(0)


if __name__ == "__main__":
    main()
