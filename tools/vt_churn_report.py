#!/usr/bin/env python3
"""Correlate VT event logs with stats markers without counting teardown releases.

Requires MATTER_VT_EVENT_LOG=1. A recorded fill is a submitted producer result,
not proof that the GPU completed it. Refills after an owner release are excluded
from eviction/refill pairs: they belong to a different owner lifetime.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re


EVENT = re.compile(r"\[vt-(release|dirty|evict|stale|page)\]\s+(.*)")
FIELD = re.compile(r"(\w+)=([^\s,]+)")


def report(path, marker_prefix=""):
    marker = ""
    sections = {}
    evicted = {}
    released = {}
    frames = Counter()
    logging_seen = False
    for line in Path(path).open(encoding="utf-8", errors="replace"):
        if "profile: wrote trace" in line or "vt-trace: wrote capture" in line:
            break  # Destruction releases every owner; that is not camera churn.
        if line.startswith("STATS,"):
            marker = line.split(",", 2)[1]
        included = marker.startswith(marker_prefix)
        section = sections.setdefault(marker, {
            "events": Counter(), "dirty_reasons": Counter(),
            "owner_retire_reasons": Counter(), "owner_return_gaps_frames": [],
            "evicted_owners": Counter(), "refill_gaps_frames": [],
            "park_timeout_warnings": 0,
        }) if included else None
        if section is not None and line.startswith("STATSVT,"):
            section["stats_at_marker"] = dict(FIELD.findall(line))
        if section is not None and "parked " in line and " s without its " in line:
            section["park_timeout_warnings"] += 1
        if section is not None and "[vt-owner-retire]" in line:
            section["owner_retire_reasons"][dict(FIELD.findall(line)).get("reason", "unknown")] += 1
        match = EVENT.search(line)
        if not match:
            continue
        logging_seen = True
        kind, payload = match.groups()
        fields = dict(FIELD.findall(payload))
        owner = fields.get("owner")
        frame = int(fields.get("frame", 0))
        key = (owner, fields.get("mip"), fields.get("x"), fields.get("y"))
        if kind == "release":
            # Release cancels the old owner, even outside the requested marker.
            evicted.pop(owner, None)
            released[owner] = frame
        elif kind == "evict":
            evicted.setdefault(owner, {})[key] = frame
        elif kind == "page" and fields.get("result") == "recorded":
            old_owner = released.pop(owner, None)
            if old_owner is not None and section is not None:
                section["owner_return_gaps_frames"].append(frame - old_owner)
            old = evicted.get(owner, {}).pop(key, None)
            if old is not None and section is not None:
                section["refill_gaps_frames"].append(frame - old)
        if section is None:
            continue
        section["events"][kind] += 1
        frames[(marker, frame)] += 1
        if kind == "dirty":
            section["dirty_reasons"][fields.get("reason", "unknown")] += 1
        elif kind == "evict":
            section["evicted_owners"][owner] += 1
        elif kind == "page":
            section["events"]["page_" + fields.get("result", "unknown")] += 1
    for name, section in sections.items():
        gaps = sorted(section.pop("refill_gaps_frames"))
        section["same_lifetime_eviction_refills"] = {
            "count": len(gaps), "within_60_frames": sum(g <= 60 for g in gaps),
            "min_frames": gaps[0] if gaps else None,
            "median_frames": gaps[len(gaps) // 2] if gaps else None,
        }
        returns = sorted(section.pop("owner_return_gaps_frames"))
        section["released_owner_returns"] = {
            "count": len(returns), "within_60_frames": sum(g <= 60 for g in returns),
            "min_frames": returns[0] if returns else None,
        }
        section["evicted_owner_count"] = len(section["evicted_owners"])
        section["most_evicted_owners"] = section.pop("evicted_owners").most_common(8)
        section["largest_event_bursts"] = Counter({
            frame: count for (label, frame), count in frames.items() if label == name
        }).most_common(5)
    return {"event_logging_seen": logging_seen, "marker_prefix": marker_prefix,
            "sections": sections}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--marker-prefix", default="")
    args = parser.parse_args()
    print(json.dumps(report(args.log, args.marker_prefix), indent=2))
