#!/usr/bin/env python3
"""Strict fixed-exposure atmosphere-presentation acceptance metrics."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

from PIL import Image


FLOAT = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
TRIPLE_RE = re.compile(
    rf"^\(({FLOAT}),\s*({FLOAT}),\s*({FLOAT})\)$"
)
PROPERTY_RE = re.compile(r"^(get|set): ([A-Za-z0-9_.]+) = (.+)$")
SHOT_RE = re.compile(r"^shot_now: queued (.+\.png)$")
CAPTURE_RE = re.compile(r"^acceptance_(raster|native_rt)_(-?\d+)\.png$")

PATHS = ("raster", "native_rt")
ELEVATIONS = (90, 5, 0, -5, -12)
STATUS_TYPES = {
    "viewer.session.render_path": "text",
    "viewer.session.presented_frame_serial": "uint",
    "viewer.session.native_rt_available": "bool",
    "viewer.atmosphere_status.generation_serial": "uint",
    "viewer.atmosphere_status.resolved_elevation_deg": "float",
    "viewer.atmosphere_status.atmospheric_direct_base_rgb": "triple",
    "viewer.atmosphere_status.atmospheric_noon_direct_base_rgb": "triple",
    "viewer.atmosphere_status.direct_world_ratio": "float",
    "viewer.atmosphere_status.direct_base_rgb": "triple",
    "viewer.atmosphere_status.direct_world_sun_rgb": "triple",
    "viewer.atmosphere_status.sky_ambient_ratio": "float",
    "viewer.atmosphere_status.sky_display_modifier_rgb": "triple",
    "viewer.atmosphere_status.sky_irradiance_modifier_rgb": "triple",
}
REQUEST_TYPES = {
    "render.lighting.exposure_ev": "float",
    "render.lighting.day_ambient_multiplier": "float",
    "render.lighting.twilight_ambient_multiplier": "float",
    "render.lighting.sky_irradiance_multiplier": "float",
    "render.lighting.sunset_direct_ratio": "float",
    "render.lighting.sun_elevation_deg": "float",
    "render.lighting.sun_multiplier": "float",
    "render.lighting.sun_tint": "triple",
}
ROIS = {
    # Fixed camera (0,2,12)->(0,1,0), Pillow top-left coordinates. These
    # rectangles have margin to the projected edges of their named surfaces.
    "noon_lit_floor": (400, 470, 440, 510),
    "noon_unlit_occluder": (615, 270, 655, 310),
    "native_five_lit_floor": (300, 500, 340, 540),
    # The first receiver row below the occluder's projected bottom is the
    # near-contact end of the long native-RT ground shadow.
    "native_five_cast_shadow": (620, 437, 660, 437),
    "upward_fog": (430, 260, 470, 290),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--capture-dir", required=True, type=Path)
    parser.add_argument("--width", required=True, type=int)
    parser.add_argument("--height", required=True, type=int)
    return parser.parse_args()


def parse_value(raw: str, kind: str):
    if kind == "triple":
        match = TRIPLE_RE.fullmatch(raw)
        if not match:
            raise ValueError(f"expected strict (r,g,b) triple, got {raw!r}")
        value = tuple(float(match.group(i)) for i in range(1, 4))
        if not all(math.isfinite(channel) for channel in value):
            raise ValueError(f"non-finite triple {raw!r}")
        return value
    if kind == "float":
        if not re.fullmatch(FLOAT, raw):
            raise ValueError(f"expected float, got {raw!r}")
        value = float(raw)
        if not math.isfinite(value):
            raise ValueError(f"non-finite float {raw!r}")
        return value
    if kind == "uint":
        if not re.fullmatch(r"\d+", raw):
            raise ValueError(f"expected uint, got {raw!r}")
        return int(raw)
    if kind == "bool":
        if raw not in ("true", "false"):
            raise ValueError(f"expected bool, got {raw!r}")
        return raw == "true"
    return raw


def smoothstep(a: float, b: float, value: float) -> float:
    q = min(1.0, max(0.0, (value - a) / (b - a)))
    return q * q * (3.0 - 2.0 * q)


def expected_direct(elevation: float, sunset: float) -> float:
    sunset = min(1.0, max(0.0, sunset))
    if elevation <= 0.0:
        return 0.0
    if elevation < 5.0:
        return sunset * smoothstep(0.0, 5.0, elevation)
    if elevation < 45.0:
        return sunset + (1.0 - sunset) * smoothstep(5.0, 45.0, elevation)
    return 1.0


def expected_ambient(elevation: float, day: float, twilight: float) -> float:
    mix = 1.0 - smoothstep(-6.0, 5.0, elevation)
    return day + (twilight - day) * mix


def luminance(rgb) -> float:
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def srgb_to_linear(code: float) -> float:
    return code / 12.92 if code <= 0.04045 else ((code + 0.055) / 1.055) ** 2.4


def invert_aces(value: float) -> float:
    # (2.43*y-2.51)x^2 + (0.59*y-0.03)x + 0.14*y = 0.
    a = 2.43 * value - 2.51
    b = 0.59 * value - 0.03
    c = 0.14 * value
    if abs(a) < 1.0e-12:
        root = -c / b if abs(b) >= 1.0e-12 else 0.0
        return max(0.0, root)
    discriminant = max(0.0, b * b - 4.0 * a * c)
    sqrt_discriminant = math.sqrt(discriminant)
    roots = ((-b + sqrt_discriminant) / (2.0 * a),
             (-b - sqrt_discriminant) / (2.0 * a))
    non_negative = [root for root in roots if root >= 0.0 and math.isfinite(root)]
    if not non_negative:
        raise ValueError(f"ACES inverse has no non-negative root for {value}")
    return min(non_negative)


def roi_metrics(image: Image.Image, roi, exposure_ev: float):
    x0, y0, x1, y1 = roi
    channels = [0.0, 0.0, 0.0]
    pixel_count = (x1 - x0 + 1) * (y1 - y0 + 1)
    non_finite = 0
    pixels = image.load()
    exposure = 2.0 ** exposure_ev
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            encoded = pixels[x, y]
            for channel in range(3):
                display_linear = srgb_to_linear(encoded[channel] / 255.0)
                scene_linear = invert_aces(display_linear) / exposure
                if not math.isfinite(scene_linear):
                    non_finite += 1
                channels[channel] += scene_linear
    mean_rgb = tuple(channel / pixel_count for channel in channels)
    return {
        "bounds_inclusive": list(roi),
        "mean_rgb": list(mean_rgb),
        "mean_luminance": luminance(mean_rgb),
        "non_finite_channels": non_finite,
    }


def parse_captures(log_path: Path):
    state = {}
    captures = {}
    parse_errors = []
    for line_number, line in enumerate(log_path.read_text(
            encoding="utf-8", errors="replace").splitlines(), 1):
        property_match = PROPERTY_RE.fullmatch(line)
        if property_match:
            path = property_match.group(2)
            kind = STATUS_TYPES.get(path) or REQUEST_TYPES.get(path)
            if kind:
                try:
                    state[path] = parse_value(property_match.group(3), kind)
                except ValueError as exc:
                    parse_errors.append(f"line {line_number}: {path}: {exc}")
            continue
        shot_match = SHOT_RE.fullmatch(line)
        if not shot_match:
            continue
        filename = Path(shot_match.group(1).replace("\\", "/")).name
        capture_match = CAPTURE_RE.fullmatch(filename)
        if not capture_match:
            continue
        key = (capture_match.group(1), int(capture_match.group(2)))
        if key in captures:
            parse_errors.append(f"duplicate capture log block for {key}")
        captures[key] = dict(state)
        captures[key]["capture_filename"] = filename
        captures[key]["shot_log_line"] = line_number
    return captures, parse_errors


def main() -> int:
    args = parse_args()
    failures = []
    captures, parse_errors = parse_captures(args.log)
    failures.extend(parse_errors)
    expected_keys = {(path, elevation) for path in PATHS for elevation in ELEVATIONS}
    missing = sorted(expected_keys - set(captures))
    extra = sorted(set(captures) - expected_keys)
    if missing:
        failures.append(f"missing capture blocks: {missing}")
    if extra:
        failures.append(f"unexpected capture blocks: {extra}")

    summary_captures = {}
    required = tuple(STATUS_TYPES) + tuple(REQUEST_TYPES)
    for path in PATHS:
        for elevation in ELEVATIONS:
            key = (path, elevation)
            if key not in captures:
                continue
            state = captures[key]
            absent = [name for name in required if name not in state]
            if absent:
                failures.append(f"{key}: missing status/requested values: {absent}")
                continue
            if state["viewer.session.render_path"] != path:
                failures.append(f"{key}: published render path mismatch")
            if path == "native_rt" and not state["viewer.session.native_rt_available"]:
                failures.append(f"{key}: native RT was not available")
            if abs(state["viewer.atmosphere_status.resolved_elevation_deg"] - elevation) > 1.0e-4:
                failures.append(f"{key}: committed elevation mismatch")
            if abs(state["render.lighting.sun_elevation_deg"] - elevation) > 1.0e-6:
                failures.append(f"{key}: requested elevation mismatch")

            direct_expected = expected_direct(
                float(elevation), state["render.lighting.sunset_direct_ratio"])
            ambient_expected = expected_ambient(
                float(elevation),
                state["render.lighting.day_ambient_multiplier"],
                state["render.lighting.twilight_ambient_multiplier"])
            direct_actual = state["viewer.atmosphere_status.direct_world_ratio"]
            ambient_actual = state["viewer.atmosphere_status.sky_ambient_ratio"]
            if abs(direct_actual - direct_expected) > 1.0e-6:
                failures.append(f"{key}: direct curve {direct_actual} != {direct_expected}")
            if abs(ambient_actual - ambient_expected) > 1.0e-6:
                failures.append(f"{key}: ambient curve {ambient_actual} != {ambient_expected}")

            noon_atmospheric = state[
                "viewer.atmosphere_status.atmospheric_noon_direct_base_rgb"]
            sun_tint = state["render.lighting.sun_tint"]
            sun_multiplier = state["render.lighting.sun_multiplier"]
            live_noon = tuple(noon_atmospheric[i] * sun_tint[i] * sun_multiplier
                              for i in range(3))
            world_sun = state["viewer.atmosphere_status.direct_world_sun_rgb"]
            noon_luma = luminance(live_noon)
            status_ratio = luminance(world_sun) / noon_luma if noon_luma > 0.0 else 0.0
            if abs(status_ratio - direct_expected) > 2.0e-3:
                failures.append(
                    f"{key}: noon-relative status ratio {status_ratio} != {direct_expected}")
            if elevation == -5:
                if direct_actual != 0.0 or any(channel != 0.0 for channel in world_sun):
                    failures.append(f"{key}: -5 direct ratio/RGB must be exactly zero")
            if state["render.lighting.exposure_ev"] != -2.0:
                failures.append(f"{key}: exposure is not exactly -2")

            image_path = args.capture_dir / state["capture_filename"]
            if not image_path.is_file():
                failures.append(f"{key}: missing PNG {image_path}")
                continue
            with Image.open(image_path) as source:
                image = source.convert("RGB")
                if image.size != (args.width, args.height):
                    failures.append(
                        f"{key}: image size {image.size} != {(args.width, args.height)}")
                if any(roi[2] >= image.width or roi[3] >= image.height
                       for roi in ROIS.values()):
                    failures.append(f"{key}: ROI is outside the capture")
                    continue
                roi_values = {
                    name: roi_metrics(image, roi, -2.0)
                    for name, roi in ROIS.items()
                }
            if sum(roi["non_finite_channels"] for roi in roi_values.values()) != 0:
                failures.append(f"{key}: non-finite recovered ROI pixel")

            summary_key = f"{path}:{elevation}"
            summary_captures[summary_key] = {
                "path": path,
                "elevation": elevation,
                "png": str(image_path.resolve()),
                "generation_serial": state[
                    "viewer.atmosphere_status.generation_serial"],
                "presented_frame_serial": state[
                    "viewer.session.presented_frame_serial"],
                "native_rt_available": state[
                    "viewer.session.native_rt_available"],
                "requested": {
                    name: state[name] for name in REQUEST_TYPES
                },
                "resolved": {
                    name: state[name] for name in STATUS_TYPES
                    if name.startswith("viewer.atmosphere_status.")
                },
                "expected_direct_world_ratio": direct_expected,
                "expected_sky_ambient_ratio": ambient_expected,
                "live_tinted_multiplied_noon_base_rgb": list(live_noon),
                "published_noon_relative_luminance_ratio": status_ratio,
                "rois": roi_values,
            }

    rgb_deltas = {}
    for elevation in ELEVATIONS:
        raster = captures.get(("raster", elevation), {}).get(
            "viewer.atmosphere_status.direct_world_sun_rgb")
        native = captures.get(("native_rt", elevation), {}).get(
            "viewer.atmosphere_status.direct_world_sun_rgb")
        if raster is None or native is None:
            continue
        delta = [abs(raster[i] - native[i]) for i in range(3)]
        rgb_deltas[str(elevation)] = delta
        if max(delta) > 2.0e-3:
            failures.append(
                f"elevation {elevation}: raster/native direct RGB delta {delta} exceeds 0.002")

    for path in PATHS:
        noon = summary_captures.get(f"{path}:90")
        twilight = summary_captures.get(f"{path}:-5")
        night = summary_captures.get(f"{path}:-12")
        if noon:
            lit = noon["rois"]["noon_lit_floor"]["mean_luminance"]
            unlit = noon["rois"]["noon_unlit_occluder"]["mean_luminance"]
            if lit < 1.10 * unlit:
                failures.append(
                    f"{path}: noon lit floor mean {lit} is below 1.10 * "
                    f"unlit occluder {unlit}")
        if twilight and twilight["rois"]["upward_fog"]["mean_luminance"] <= 1.0e-4:
            failures.append(f"{path}: -5 upward/fog mean is not positive")
        if twilight and night:
            twilight_fog = twilight["rois"]["upward_fog"]["mean_luminance"]
            night_fog = night["rois"]["upward_fog"]["mean_luminance"]
            if not night_fog < twilight_fog:
                failures.append(
                    f"{path}: -12 upward/fog mean {night_fog} is not below -5 {twilight_fog}")

    native_five = summary_captures.get("native_rt:5")
    if native_five:
        lit = native_five["rois"]["native_five_lit_floor"]["mean_luminance"]
        shadow = native_five["rois"][
            "native_five_cast_shadow"]["mean_luminance"]
        if lit < 1.10 * shadow:
            failures.append(
                f"native_rt: +5 lit floor mean {lit} is below 1.10 * "
                f"cast ground shadow {shadow}")

    output = args.capture_dir / "atmosphere_presentation_metrics.json"
    result = {
        "gate": "PASS" if not failures else "FAIL",
        "log": str(args.log.resolve()),
        "capture_dir": str(args.capture_dir.resolve()),
        "dimensions": [args.width, args.height],
        "captures": summary_captures,
        "raster_native_rt_direct_rgb_delta": rgb_deltas,
        "failures": failures,
    }
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(f"{result['gate']}: {output.resolve()}")
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
