#!/usr/bin/env python3
"""Analyze a complete MATTER_VT_DENSITY_FRAME diagnostic, never a timing gate."""

import argparse
import json
from pathlib import Path
import re


def analyze(text):
    begin, end, pages = None, None, []
    for line in text.splitlines():
        fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
        if "[vt-density] begin " in line:
            if begin is not None:
                raise ValueError("Expected one density snapshot per input log")
            begin = {k: int(v) for k, v in fields.items()}
        elif "[vt-density] end " in line:
            if begin is None or end is not None:
                raise ValueError("Unexpected density snapshot end")
            end = {k: int(v) for k, v in fields.items()}
        elif "[vt-density-page] " in line:
            if begin is None or end is not None:
                raise ValueError("Page outside density snapshot")
            pages.append({k: int(v, 16 if k == "owner" else 10) for k, v in fields.items()})
    if begin is None or end is None or begin.get("schema") != 1:
        raise ValueError("Missing, incomplete, or unsupported density snapshot")
    if end["frame"] != begin["frame"] or not (
        end["reported"] == end["occupied"] == begin["occupied"] == len(pages)
    ):
        raise ValueError("Density page count or frame mismatch")
    if len({p["slot"] for p in pages}) != len(pages):
        raise ValueError("Duplicate physical slot in density snapshot")
    capacity, payload, stride = begin["capacity"], begin["payload"], begin["stride"]
    reserved = begin.get("reserved", 0)
    resident_capacity = begin.get("resident_capacity", capacity)
    if reserved < 0 or resident_capacity + reserved != capacity or len(pages) > resident_capacity:
        raise ValueError("Invalid reserved/resident pool capacity")
    if not 0 < payload <= stride or not 0 <= len(pages) <= capacity:
        raise ValueError("Invalid pool geometry")
    if sum(p["pinned"] for p in pages) != begin["pinned"]:
        raise ValueError("Pinned page count mismatch")
    for p in pages:
        if not (0 <= p["slot"] < capacity and p["pinned"] in (0, 1) and
                p["geometry"] in (0, 1) and p["tail_filled"] in (0, 1)):
            raise ValueError("Invalid page identity or status")
        if not 0 <= p["block"] <= p["atlas"] <= payload**2:
            raise ValueError("Chart blocks exceed atlas/page coverage")
        if p["geometry"] and not 0 <= p["triangle"] <= p["bounds"] <= p["gutter"] <= p["block"]:
            raise ValueError("Geometry coverage is outside nested chart bounds")

    def group(selected):
        count = len(selected)
        total = count * payload**2
        sums = {k: sum(p[k] for p in selected) for k in
                ("atlas", "block", "bounds", "gutter", "triangle")}
        missing = sum(not p["geometry"] for p in selected)
        complete = not missing
        return {
            "pages": count,
            "geometry_unavailable_pages": missing,
            "unfilled_tail_pages": sum(p["pinned"] and not p["tail_filled"] for p in selected),
            "payload_texels": total,
            "atlas_texels": sums["atlas"],
            "chart_block_texels": sums["block"],
            "triangle_covered_texels": sums["triangle"] if complete else None,
            "triangle_fraction_of_payload": sums["triangle"] / total if complete and total else None,
            "triangle_fraction_of_stored_slot": sums["triangle"] / (count * stride**2) if complete and count else None,
            "payload_partition": {
                "outside_atlas": total - sums["atlas"],
                "between_chart_blocks": sums["atlas"] - sums["block"],
                "block_padding_outside_gutter_bounds": sums["block"] - sums["gutter"] if complete else None,
                "gutter_bounds_outside_content_bounds": sums["gutter"] - sums["bounds"] if complete else None,
                "inside_content_bounds_outside_triangles": sums["bounds"] - sums["triangle"] if complete else None,
                "triangle_covered": sums["triangle"] if complete else None,
            },
        }

    return {
        "schema": 1,
        "frame": begin["frame"],
        "pool_capacity_pages": capacity,
        "replacement_reserve_pages": reserved,
        "resident_capacity_pages": resident_capacity,
        "occupied_pages": len(pages),
        "occupied_fraction": len(pages) / capacity if capacity else None,
        "pool_format_bytes": begin["format_bytes"],
        "occupied_slot_format_bytes": begin["format_bytes"] * len(pages) // capacity if capacity else 0,
        "physical_border_fraction_of_slot": 1 - payload**2 / stride**2,
        "groups": {
            "all": group(pages),
            "pinned": group([p for p in pages if p["pinned"]]),
            "detail": group([p for p in pages if not p["pinned"]]),
        },
        "limits": [
            "CPU geometric center coverage after command recording, not a GPU completion or visible-pixel metric.",
            "Counts occupied cached pages, including offscreen content; aliases count once per physical slot.",
            "Filtering and dilation need some uncovered texels; these are not all reclaimable bytes.",
            "Page partitions use one payload denominator and exclude physical borders, which are reported separately.",
            "Pool format bytes exclude allocation alignment and other VT resources.",
            "This costly diagnostic is excluded from timing acceptance.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = analyze(args.log.read_text(encoding="utf-8", errors="replace"))
    except (ValueError, KeyError) as error:
        parser.error(str(error))
    result["log"] = str(args.log)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
