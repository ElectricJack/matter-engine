#!/usr/bin/env python3
"""Finite, fail-closed native object capture and audited-mask scoring.

See docs/agent/object-evaluation.md for the versioned input and receipt format.
"""
from __future__ import annotations

import argparse
from array import array
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import struct
import sys
import tempfile
import threading
import time
import uuid

from PIL import Image, ImageChops, ImageStat

import matter_agent

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 1
MODEL_PATH = re.compile(r"^projects/world_demo/(objects/[^/]+(?:/[^/]+)*/[^/]+\.js|scenes/[^/]+/[^/]+/(objects/[^/]+\.js|presets/[^/]+\.json)|materials/[^/]+\.js)$")
VIEW_FILE = re.compile(r"[A-Za-z0-9_.-]+\.png")
PLANE_COUNT = 6
IDENTITY_PLANE = "identity"
IDENTITY_FORMAT = "rg32_uint"
IDENTITY_BYTES_PER_PIXEL = 8
IDENTITY_BACKGROUND = 0xFFFFFFFF
# A silhouette that covers more of the frame than this is a coverage or
# background failure, not an object.
MAX_MASK_COVERAGE = .9


class EvaluationError(Exception):
    pass


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                      allow_nan=False).encode("utf-8")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_hash(path: Path) -> str:
    return digest(path.read_bytes())


def load_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or value.get("version") != SCHEMA:
        raise EvaluationError(f"{path}: expected version {SCHEMA} object")
    return value


def relative_file(raw: str) -> Path:
    path = Path(raw)
    if path.is_absolute() or not path.parts or any(p in (".", "..") for p in path.parts):
        raise EvaluationError(f"unsafe source path: {raw}")
    resolved = (ROOT / path).resolve()
    if not resolved.is_relative_to(ROOT) or not resolved.is_file():
        raise EvaluationError(f"source missing or outside repo: {raw}")
    return path


def validate_manifest(manifest: dict) -> None:
    if not isinstance(manifest.get("world"), str) or not manifest["world"]:
        raise EvaluationError("world is required")
    if not isinstance(manifest.get("expected_root_module"), str) or not manifest["expected_root_module"]:
        raise EvaluationError("expected_root_module is required")
    if not isinstance(manifest.get("expected_root_params"), dict):
        raise EvaluationError("expected_root_params must be an object")
    sources = manifest.get("source_paths")
    writable = manifest.get("model_write_paths")
    if not isinstance(sources, list) or not sources or not all(isinstance(x, str) for x in sources):
        raise EvaluationError("source_paths must be a nonempty string list")
    if len(set(sources)) != len(sources):
        raise EvaluationError("duplicate source path")
    for raw in sources:
        relative_file(raw)
    if not isinstance(writable, list) or not writable or not set(writable) <= set(sources):
        raise EvaluationError("model_write_paths must be a nonempty subset of source_paths")
    if any(not MODEL_PATH.fullmatch(x) for x in writable):
        raise EvaluationError("model write path is outside generator/material/preset scope")
    rig = manifest.get("rig")
    if not isinstance(rig, dict) or not isinstance(rig.get("views"), list) or not rig["views"]:
        raise EvaluationError("rig.views is required")
    names = set()
    for view in rig["views"]:
        if not isinstance(view, dict) or not re.fullmatch(r"[A-Za-z0-9_-]{1,40}", str(view.get("name", ""))):
            raise EvaluationError("invalid view name")
        if view["name"] in names:
            raise EvaluationError("duplicate view name")
        names.add(view["name"])
        pose = view.get("pose")
        if not isinstance(pose, list) or len(pose) != 6 or any(type(v) not in (int, float) or not math.isfinite(v) for v in pose):
            raise EvaluationError("view pose must be six finite numbers")
        up = view.get("up", [0, 1, 0])
        if not isinstance(up, list) or len(up) != 3 or any(type(v) not in (int, float) or not math.isfinite(v) for v in up):
            raise EvaluationError("view up must be three finite numbers")
    if type(rig.get("hold_frames")) is not int or rig["hold_frames"] < 1:
        raise EvaluationError("hold_frames must be positive")
    if type(rig.get("desired_max_lod", 0)) is not int or not 0 <= rig.get("desired_max_lod", 0) <= 7:
        raise EvaluationError("desired_max_lod must be an integer from 0 to 7")
    projection = rig.get("projection")
    if not isinstance(projection, dict) or set(projection) != {"vertical_fov_radians", "near_plane", "far_plane"}:
        raise EvaluationError("projection must fix fov, near and far")
    if any(type(v) not in (int, float) or not math.isfinite(v) or v <= 0 for v in projection.values()):
        raise EvaluationError("invalid projection")
    if not 0.05 <= projection["vertical_fov_radians"] <= 3 or not 0.001 <= projection["near_plane"] < projection["far_plane"] <= 1e7:
        raise EvaluationError("projection is outside native camera bounds")


def freeze(manifest_path: Path, bundle: Path) -> dict:
    manifest = load_json(manifest_path)
    validate_manifest(manifest)
    for raw in manifest["source_paths"]:
        if raw.endswith(".js"):
            syntax = subprocess.run(["node", "--check", str(ROOT / raw)],
                                    capture_output=True, text=True)
            if syntax.returncode:
                raise EvaluationError(f"invalid JS in {raw}: {syntax.stderr.strip()}")
    # Exclusive creation plus hashes makes candidate records append-only. Capture
    # rechecks the live repository against these exact bytes before launching.
    bundle.mkdir(parents=True, exist_ok=False)
    files = {raw: file_hash(ROOT / raw) for raw in sorted(manifest["source_paths"])}
    record = {"version": SCHEMA, "manifest": manifest, "manifest_sha256": digest(canonical(manifest)),
              "rig_sha256": digest(canonical(manifest["rig"])), "source_sha256": files,
              "source_closure_sha256": digest(canonical(files))}
    record["candidate_sha256"] = digest(canonical(record))
    (bundle / "sources").mkdir()
    for raw in files:
        dest = bundle / "sources" / raw
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes((ROOT / raw).read_bytes())
    (bundle / "candidate.json").write_bytes(canonical(record) + b"\n")
    return record


def verify_bundle(bundle: Path) -> dict:
    record = load_json(bundle / "candidate.json")
    candidate_hash = record.pop("candidate_sha256", None)
    if candidate_hash != digest(canonical(record)):
        raise EvaluationError("candidate record was changed")
    record["candidate_sha256"] = candidate_hash
    validate_manifest(record["manifest"])
    if record["manifest_sha256"] != digest(canonical(record["manifest"])) or record["rig_sha256"] != digest(canonical(record["manifest"]["rig"])):
        raise EvaluationError("manifest or rig changed")
    files = record["source_sha256"]
    if files != {raw: file_hash(ROOT / raw) for raw in sorted(record["manifest"]["source_paths"])}:
        raise EvaluationError("live source closure differs from candidate")
    if files != {raw: file_hash(bundle / "sources" / raw) for raw in files}:
        raise EvaluationError("frozen source closure changed")
    return record


def native_path(path: Path) -> str:
    path = path.resolve()
    if os.name == "nt":
        return str(path)
    parts = path.parts
    if len(parts) < 4 or parts[1] != "mnt" or len(parts[2]) != 1:
        raise EvaluationError("native capture output must reside on a /mnt/<drive>/ path")
    return f"{parts[2].upper()}:/" + "/".join(parts[3:])


class Editor:
    def __init__(self, run_dir: Path, world: str, executable: Path):
        self.run_dir = run_dir
        self.command_file = run_dir / "commands.txt"
        self.result_file = run_dir / "results.jsonl"
        self.command_file.touch()
        self.result_file.touch()
        env = os.environ.copy()
        env.update(MATTER_WORLD=world, MATTER_CMD_FIFO=native_path(self.command_file),
                   MATTER_AGENT_RESULT_FILE=native_path(self.result_file), MATTER_HIDE_UI="1")
        if os.name != "nt":
            native_temp = subprocess.check_output(["/mnt/c/Windows/System32/cmd.exe", "/c", "echo", "%TEMP%"], text=True).strip()
            env.update(TMP=native_temp, TEMP=native_temp)
            exported = "MATTER_WORLD:MATTER_CMD_FIFO:MATTER_AGENT_RESULT_FILE:MATTER_HIDE_UI:TMP:TEMP"
            env["WSLENV"] = env.get("WSLENV", "") + (":" if env.get("WSLENV") else "") + exported
        else:
            env.update(TMP=tempfile.gettempdir(), TEMP=tempfile.gettempdir())
        self.log = (run_dir / "editor.log").open("w", encoding="utf-8")
        self.lines: list[str] = []
        self.lock = threading.Lock()
        self.proc = subprocess.Popen([str(executable)], cwd=str(ROOT / "MatterEditor"), env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, errors="replace", bufsize=1)
        self.pump = threading.Thread(target=self._pump, daemon=True)
        self.pump.start()

    def _pump(self):
        for line in self.proc.stdout:
            self.log.write(line)
            self.log.flush()
            with self.lock:
                self.lines.append(line.rstrip("\r\n"))

    def send_fifo(self, lines: str) -> None:
        with self.command_file.open("a", encoding="utf-8") as stream:
            stream.write(lines)
            stream.flush()
            os.fsync(stream.fileno())

    def await_line(self, pattern: str, start: int, deadline: float) -> None:
        while time.monotonic() < deadline:
            with self.lock:
                if any(pattern in line for line in self.lines[start:]):
                    return
            if self.proc.poll() is not None:
                raise EvaluationError(f"editor exited {self.proc.returncode} before {pattern}")
            time.sleep(.05)
        raise EvaluationError(f"timed out waiting for {pattern}")

    def request(self, command: str, args: dict, timeout: float = 30,
                accepted_codes: tuple[str, ...] = ("ok",)) -> dict:
        envelope = matter_agent.make_envelope(command, args, {}, str(uuid.uuid4()), int(timeout * 1000))
        result = matter_agent.submit_request(self.command_file, self.result_file, envelope, timeout)
        if result.get("code") not in accepted_codes:
            raise EvaluationError(f"{command}: {result.get('code')}: {result.get('message')}")
        return result

    def close(self):
        if self.proc.poll() is None:
            self.send_fifo("quit\n")
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.pump.join(timeout=2)
        self.log.close()


def check_png(path: Path, result: dict) -> dict:
    marker = Path(str(path) + ".done")
    if not path.is_file() or not marker.is_file() or marker.read_text().strip() != "captured":
        raise EvaluationError(f"missing capture or completion marker: {path}")
    with Image.open(path) as image:
        image.load()
        if image.format != "PNG" or image.size != (result["image"]["width"], result["image"]["height"]):
            raise EvaluationError(f"invalid PNG dimensions: {path}")
        if image.width < 32 or image.height < 32 or max(ImageStat.Stat(image.convert("RGB")).stddev) < 1:
            raise EvaluationError(f"empty foreground candidate: {path}")
        size = list(image.size)
    return {"path": str(path), "sha256": file_hash(path), "width": size[0], "height": size[1]}


def check_channels(path: Path, result: dict) -> dict:
    channels = result.get("channels", {})
    if channels.get("available") is not True or channels.get("container") != "MECAP001":
        raise EvaluationError("capture has no numeric channels")
    if channels.get("path", "").replace("\\", "/").lower() != native_path(path).lower():
        raise EvaluationError("channel path differs from requested capture")
    if not path.is_file():
        raise EvaluationError("numeric channel bundle is missing")
    with path.open("rb") as stream:
        header = stream.read(16)
    if len(header) != 16 or header[:8] != b"MECAP001":
        raise EvaluationError("invalid numeric channel header")
    width, height = struct.unpack("<II", header[8:])
    if (width, height) != (channels["width"], channels["height"]):
        raise EvaluationError("numeric channel dimensions differ from receipt")
    planes = channels.get("planes", [])
    end = 16
    for plane in planes:
        if int(plane["offset_bytes"]) != end:
            raise EvaluationError("numeric channel plane offsets are not contiguous")
        end += int(plane["size_bytes"])
    if len(planes) != PLANE_COUNT or path.stat().st_size != end:
        raise EvaluationError("numeric channel bundle is incomplete")
    return {"path": str(path), "sha256": file_hash(path), "width": width, "height": height}


def identity_coverage(channels_path: Path, channels: dict) -> tuple[int, int, list[bool]]:
    """Object coverage of one capture from its own identity plane: UINT32_MAX in
    both lanes is background, any other pair is a rasterized pixel of some
    object. Nothing here infers coverage from rendered color."""
    width, height = int(channels["width"]), int(channels["height"])
    if width < 1 or height < 1:
        raise EvaluationError("numeric channel dimensions must be positive")
    planes = [plane for plane in channels.get("planes", []) if plane.get("name") == IDENTITY_PLANE]
    if len(planes) != 1 or planes[0].get("format") != IDENTITY_FORMAT:
        raise EvaluationError("channel bundle has no single rg32_uint identity plane")
    plane = planes[0]
    expected = width * height * IDENTITY_BYTES_PER_PIXEL
    if int(plane["size_bytes"]) != expected:
        raise EvaluationError("identity plane size differs from the channel grid")
    with channels_path.open("rb") as stream:
        stream.seek(int(plane["offset_bytes"]))
        raw = stream.read(expected)
    if len(raw) != expected:
        raise EvaluationError("identity plane is truncated")
    lanes = array("I")
    lanes.frombytes(raw)
    if lanes.itemsize != 4:
        raise EvaluationError("host integers are not 32 bits")
    if sys.byteorder != "little":
        lanes.byteswap()
    return width, height, [(material != IDENTITY_BACKGROUND) or (token != IDENTITY_BACKGROUND)
                           for material, token in zip(lanes[0::2], lanes[1::2])]


def resample_mask(covered: list[bool], source: tuple[int, int],
                  target: tuple[int, int]) -> list[bool]:
    """Nearest-neighbour transfer of coverage between two raster grids. The
    internal extent can differ from the display PNG when scaling is active."""
    if source == target:
        return list(covered)
    source_width, source_height = source
    target_width, target_height = target
    columns = [(x * source_width) // target_width for x in range(target_width)]
    return [covered[base + column]
            for base in ((y * source_height) // target_height * source_width for y in range(target_height))
            for column in columns]


def mask_bounds(mask: list[bool], size: tuple[int, int]) -> tuple[dict | None, int]:
    """Bounding box and set-pixel count of a mask. Every index is inside the
    frame by construction; the box reports what the mask claims."""
    width, height = size
    set_pixels = [index for index, value in enumerate(mask) if value]
    if not set_pixels:
        return None, 0
    columns = [index % width for index in set_pixels]
    rows = [index // width for index in set_pixels]
    return ({"min_x": min(columns), "max_x": max(columns),
             "min_y": min(rows), "max_y": max(rows)}, len(set_pixels))


def audit_mask(mask: list[bool], size: tuple[int, int], channel_size: tuple[int, int]) -> dict:
    """Mechanical audit of one derived mask. Nonempty, inside the frame, on the
    channel raster grid, and inside a sane area bound."""
    if len(mask) != size[0] * size[1]:
        raise EvaluationError("derived mask does not cover exactly one frame")
    bounds, count = mask_bounds(mask, size)
    coverage = round(count / (size[0] * size[1]), 6)
    checks = {"nonempty": count > 0,
              "inside_frame": bounds is not None and 0 <= bounds["min_x"] <= bounds["max_x"] < size[0]
                               and 0 <= bounds["min_y"] <= bounds["max_y"] < size[1],
              "matches_channel_dimensions": size == channel_size,
              "area_within_bounds": 0 < coverage <= MAX_MASK_COVERAGE}
    return {"audited": all(checks.values()),
            "failures": sorted(name for name, passed in checks.items() if not passed),
            "width": size[0], "height": size[1],
            "channel_width": channel_size[0], "channel_height": channel_size[1],
            "resampled_to_image": size != channel_size,
            "object_pixels": count, "coverage_fraction": coverage, "bounds": bounds,
            **checks}


def emit_candidate_mask(run_dir: Path, view_name: str, channels_path: Path, channels: dict,
                        image_size: tuple[int, int]) -> dict:
    """Derive one view's candidate mask from the capture's identity plane, write
    it beside the image and audit it. The written file is what the scorer later
    reads, so it is re-read before the audit is recorded."""
    width, height, covered = identity_coverage(channels_path, channels)
    mask = resample_mask(covered, (width, height), image_size)
    path = run_dir / f"{view_name}.mask.png"
    if path.exists():
        raise EvaluationError(f"stale mask at unique capture path: {path}")
    expected = bytes(255 if value else 0 for value in mask)
    Image.frombytes("L", image_size, expected).save(path, format="PNG")
    with Image.open(path) as image:
        image.load()
        if image.format != "PNG" or image.mode != "L" or image.size != image_size:
            raise EvaluationError(f"mask is not an 8-bit grayscale PNG at capture size: {path}")
        if image.tobytes() != expected:
            raise EvaluationError(f"mask PNG does not hold the derived coverage: {path}")
    audit = audit_mask(mask, image_size, (width, height))
    audit["path"] = str(path)
    audit["sha256"] = file_hash(path)
    return audit


def capture(bundle: Path, run_dir: Path, editor_exe: Path, timeout: float) -> dict:
    candidate = verify_bundle(bundle)
    if not editor_exe.is_file():
        raise EvaluationError(f"native editor missing: {editor_exe}")
    run_dir.mkdir(parents=True, exist_ok=False)
    manifest = candidate["manifest"]
    receipt = {"version": SCHEMA, "run_id": str(uuid.uuid4()), "status": "failed", "candidate_sha256": candidate["candidate_sha256"],
               "rig_sha256": candidate["rig_sha256"], "editor_sha256": file_hash(editor_exe),
               "views": {}, "timing_seconds": {}, "readiness": "pending"}
    start = time.monotonic()
    editor = None
    try:
        editor = Editor(run_dir, manifest["world"], editor_exe)
        editor.await_line("viewer: bake ready", 0, start + timeout)
        receipt["timing_seconds"]["initial_bake"] = time.monotonic() - start
        t = time.monotonic()
        started = editor.request("job.start", {"operation": "reload"})
        receipt["job_start"] = started
        job_id = started["result"]["job"]["job_id"]
        while True:
            waited = editor.request("job.wait", {"job_id": job_id},
                                    timeout=min(30, max(1, timeout)),
                                    accepted_codes=("ok", "timeout", "execution_failure"))
            receipt["job_wait"] = waited
            state = waited["result"]["job"]["state"]
            if state == "completed":
                break
            if state in ("failed", "cancelled", "superseded") or waited.get("code") == "execution_failure":
                raise EvaluationError(f"reload bake {state}: {waited.get('message')}")
            if time.monotonic() - t > timeout:
                raise EvaluationError("bake did not complete within deadline")
        receipt["timing_seconds"]["reload_bake"] = time.monotonic() - t
        if waited["result"]["job"]["inputs"]["world"] != manifest["world"]:
            raise EvaluationError("reload job world differs from manifest")
        scene = editor.request("scene.list_objects", {"kinds": ["baked_root"], "limit": 2})
        receipt["scene"] = scene
        roots = scene["result"]["objects"]
        if scene["result"]["page"]["total_matched"] != 1 or len(roots) != 1:
            raise EvaluationError(f"expected one baked root, found {scene['result']['page']['total_matched']}")
        if roots[0].get("provenance", {}).get("module") != manifest["expected_root_module"]:
            raise EvaluationError("baked root module differs from manifest")
        params = roots[0].get("provenance", {}).get("params_json", {})
        if params.get("available") is not True or json.loads(params["value"]) != manifest["expected_root_params"]:
            raise EvaluationError("baked root parameters differ from persisted fixture")
        for view in manifest["rig"]["views"]:
            t = time.monotonic()
            pose = view["pose"]
            camera_args = {"position": pose[:3], "target": pose[3:], "up": view.get("up", [0, 1, 0]),
                           **manifest["rig"]["projection"]}
            receipt.setdefault("camera_set", {})[view["name"]] = editor.request("view.set_camera", camera_args)
            editor.request("render.reset_temporal", {})
            with editor.lock:
                offset = len(editor.lines)
            token = "object_eval_" + uuid.uuid4().hex
            editor.send_fifo(f"wait_frames {manifest['rig']['hold_frames']}\nstats {token}\n")
            editor.await_line(f"STATS,{token},", offset, time.monotonic() + timeout)
            receipt["timing_seconds"][f"{view['name']}.settle"] = time.monotonic() - t
            path = run_dir / f"{view['name']}.png"
            if path.exists() or Path(str(path) + ".done").exists():
                raise EvaluationError("stale image at unique capture path")
            result = editor.request("viewport.capture", {"path": native_path(path),
                                                         "export_channels": True,
                                                         "desired_max_lod": manifest["rig"].get("desired_max_lod", 0)}, timeout=30)
            receipt["views"][view["name"]] = {"capture": result}
            raw = result["result"]
            if raw.get("captured", {}).get("scene", {}).get("ready") is not True or raw.get("viewport", {}).get("production_view") is not True:
                raise EvaluationError("capture lacks ready production frame")
            if raw.get("presented", {}).get("id") != raw["captured"]["frame"]["id"]:
                raise EvaluationError("capture frame IDs disagree")
            if raw.get("readiness", {}).get("ready") is not True:
                raise EvaluationError(f"visible detail is not ready: {raw.get('readiness', {}).get('blockers')}")
            camera = raw["camera"]
            for observed, expected in zip(camera["position"] + camera["target"] + camera["up"],
                                          pose + camera_args["up"]):
                if not math.isfinite(observed) or abs(observed - expected) > 1e-3:
                    raise EvaluationError("capture camera differs from rig")
            for key, expected in manifest["rig"]["projection"].items():
                if not math.isfinite(camera[key]) or abs(camera[key] - expected) > 1e-3:
                    raise EvaluationError("capture projection differs from rig")
            image = check_png(path, raw)
            channel_path = Path(str(path) + ".channels.bin")
            channels = check_channels(channel_path, raw)
            mask_audited = emit_candidate_mask(run_dir, view["name"], channel_path, raw["channels"],
                                               (image["width"], image["height"]))
            receipt["views"][view["name"]].update(image=image, channels=channels,
                                                  mask_audited=mask_audited)
            if not mask_audited["audited"]:
                raise EvaluationError(f"candidate mask audit failed for {view['name']}: "
                                      f"{', '.join(mask_audited['failures'])}")
            receipt["timing_seconds"][f"{view['name']}.capture"] = time.monotonic() - t
        verify_bundle(bundle)
        if file_hash(editor_exe) != receipt["editor_sha256"]:
            raise EvaluationError("native editor changed during capture")
        receipt["readiness"] = "captured_visible_detail_ready"
        receipt["status"] = "complete"
    except Exception as exc:
        receipt["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if editor:
            editor.close()
        receipt["timing_seconds"]["total"] = time.monotonic() - start
        (run_dir / "capture.json").write_bytes(canonical(receipt) + b"\n")
    return receipt


def mask_pixels(path: Path, size: tuple[int, int]) -> list[bool]:
    with Image.open(path) as image:
        image.load()
        if image.format != "PNG" or image.mode != "L":
            raise EvaluationError(f"mask must be an 8-bit grayscale PNG: {path}")
        if image.size != size:
            raise EvaluationError(f"mask size mismatch: {path}")
        values = image.tobytes()
    if any(v not in (0, 255) for v in values) or not any(values):
        raise EvaluationError(f"mask must be nonempty binary 0/255: {path}")
    return [v == 255 for v in values]


def view_file(directory: Path, raw: object, label: str) -> Path:
    """Resolve one named PNG inside a run or reference directory. Both are
    untrusted inputs, so a path that is not a plain filename in that directory
    is refused instead of followed."""
    if not isinstance(raw, str) or not VIEW_FILE.fullmatch(raw):
        raise EvaluationError(f"{label} must be a plain PNG filename: {raw!r}")
    path = directory / raw
    if not path.is_file():
        raise EvaluationError(f"{label} is missing: {raw}")
    return path


def audited_mask(run_dir: Path, row: dict, name: str) -> dict:
    """The capture receipt's own audit for one view, or a refusal. Scoring only
    ever reads a mask the capture already derived and audited."""
    audit = row.get("mask_audited")
    if not isinstance(audit, dict) or audit.get("audited") is not True:
        raise EvaluationError(f"candidate mask was not audited: {name}")
    path = run_dir / f"{name}.mask.png"
    if not path.is_file() or file_hash(path) != audit.get("sha256"):
        raise EvaluationError(f"audited candidate mask is missing or changed: {name}")
    if (audit.get("width"), audit.get("height")) != (row.get("image", {}).get("width"),
                                                    row.get("image", {}).get("height")):
        raise EvaluationError(f"audited mask size differs from the capture image: {name}")
    return audit


def contour(mask: list[bool], width: int, height: int) -> list[int]:
    return [i for i, value in enumerate(mask) if value and (
        (i % width == 0 or not mask[i - 1]) or
        (i % width == width - 1 or not mask[i + 1]) or
        (i < width or not mask[i - width]) or
        (i >= width * (height - 1) or not mask[i + width]))]


def contour_distance(a: list[bool], b: list[bool], width: int, height: int) -> float:
    """Symmetric 8-connected chamfer distance, normalized by image diagonal."""
    edge_a, edge_b = contour(a, width, height), contour(b, width, height)
    if not edge_a or not edge_b:
        raise EvaluationError("mask has no contour")
    n = width * height
    diagonal = math.sqrt(2)

    def distance_map(edges):
        distances = [float("inf")] * n
        for i in edges:
            distances[i] = 0.0
        for y in range(height):
            base = y * width
            for x in range(width):
                i = base + x
                best = distances[i]
                if x:
                    best = min(best, distances[i - 1] + 1)
                if y:
                    best = min(best, distances[i - width] + 1)
                    if x:
                        best = min(best, distances[i - width - 1] + diagonal)
                    if x + 1 < width:
                        best = min(best, distances[i - width + 1] + diagonal)
                distances[i] = best
        for y in range(height - 1, -1, -1):
            base = y * width
            for x in range(width - 1, -1, -1):
                i = base + x
                best = distances[i]
                if x + 1 < width:
                    best = min(best, distances[i + 1] + 1)
                if y + 1 < height:
                    best = min(best, distances[i + width] + 1)
                    if x:
                        best = min(best, distances[i + width - 1] + diagonal)
                    if x + 1 < width:
                        best = min(best, distances[i + width + 1] + diagonal)
                distances[i] = best
        return distances

    to_b = distance_map(edge_b)
    to_a = distance_map(edge_a)
    return .5 * (sum(to_b[i] for i in edge_a) / len(edge_a) +
                 sum(to_a[i] for i in edge_b) / len(edge_b)) / math.hypot(width, height)


def score(run_dir: Path, reference_path: Path) -> dict:
    capture_record = load_json(run_dir / "capture.json")
    reference = load_json(reference_path)
    if capture_record.get("status") != "complete":
        raise EvaluationError("capture is incomplete; quality unavailable")
    if not isinstance(capture_record.get("run_id"), str) or not capture_record["run_id"]:
        raise EvaluationError("capture run_id is missing")
    if reference.get("rig_sha256") != capture_record["rig_sha256"]:
        raise EvaluationError("reference rig hash differs")
    expected = set(reference.get("views", {}))
    if not expected or expected != set(capture_record["views"]):
        raise EvaluationError("reference/capture view coverage differs")
    rows = {}
    for name in sorted(expected):
        row = capture_record["views"][name]
        image_path = run_dir / f"{name}.png"
        if file_hash(image_path) != row["image"]["sha256"]:
            raise EvaluationError(f"capture image changed: {name}")
        view = reference["views"][name]
        audited_mask(run_dir, row, name)
        with Image.open(image_path) as image:
            image.load()
            size = image.size
            candidate_rgb = image.convert("RGB")
        reference_image = view_file(reference_path.parent, view.get("image"), f"reference image {name}")
        with Image.open(reference_image) as image:
            image.load()
            if image.size != size:
                raise EvaluationError(f"reference dimensions differ: {name}")
            target_rgb = image.convert("RGB")
        if file_hash(reference_image) != view["image_sha256"]:
            raise EvaluationError(f"reference image changed: {name}")
        candidate_mask_path = view_file(run_dir, view.get("candidate_mask"), f"candidate mask {name}")
        if candidate_mask_path.name != f"{name}.mask.png":
            raise EvaluationError(f"candidate mask is not this run's audited mask: {name}")
        target_mask_path = view_file(reference_path.parent, view.get("reference_mask"), f"reference mask {name}")
        if file_hash(target_mask_path) != view["reference_mask_sha256"]:
            raise EvaluationError(f"reference mask changed: {name}")
        candidate_mask = mask_pixels(candidate_mask_path, size)
        target_mask = mask_pixels(target_mask_path, size)
        intersection = sum(a and b for a, b in zip(candidate_mask, target_mask))
        union = sum(a or b for a, b in zip(candidate_mask, target_mask))
        iou = intersection / union
        # Appearance is diagnostic and measured only where both audited masks
        # agree; it cannot compensate for a missing silhouette.
        diff = ImageChops.difference(candidate_rgb, target_rgb)
        rgb_bytes = diff.tobytes()
        color = sum(sum(rgb_bytes[3*i:3*i+3]) for i, (a, b) in enumerate(zip(candidate_mask, target_mask)) if a and b)
        color_mae = color / (intersection * 3 * 255) if intersection else None
        rows[name] = {"iou": iou, "contour_distance_diagonal": contour_distance(candidate_mask, target_mask, *size),
                      "color_mae_intersection": color_mae,
                      "candidate_mask_sha256": file_hash(candidate_mask_path),
                      "reference_image_sha256": view["image_sha256"],
                      "reference_mask_sha256": view["reference_mask_sha256"]}
    ious = [r["iou"] for r in rows.values()]
    colors = [r["color_mae_intersection"] for r in rows.values() if r["color_mae_intersection"] is not None]
    result = {"version": SCHEMA, "run_id": capture_record["run_id"], "status": "complete", "candidate_sha256": capture_record["candidate_sha256"],
              "rig_sha256": capture_record["rig_sha256"], "reference_sha256": digest(canonical(reference)),
              "per_view": rows, "mean_iou": sum(ious) / len(ious), "worst_iou": min(ious),
              "mean_color_mae_intersection": sum(colors) / len(colors) if colors else None,
              "loss": 1 - sum(ious) / len(ious)}
    (run_dir / "score.json").write_bytes(canonical(result) + b"\n")
    return result


def noise(score_paths: list[Path]) -> dict:
    if len(score_paths) < 5:
        raise EvaluationError("at least five unchanged scores are required")
    if len({path.resolve() for path in score_paths}) != len(score_paths):
        raise EvaluationError("noise scores must come from distinct runs")
    scores = [load_json(path) for path in score_paths]
    run_ids = [row.get("run_id") for row in scores]
    if any(not isinstance(value, str) or not value for value in run_ids) or len(set(run_ids)) != len(run_ids):
        raise EvaluationError("noise scores must have distinct capture run IDs")
    keys = ("candidate_sha256", "rig_sha256", "reference_sha256")
    if any(row.get("status") != "complete" or any(row.get(k) != scores[0].get(k) for k in keys) for row in scores):
        raise EvaluationError("noise scores must have identical candidate, rig and reference")
    differences = sorted(abs(row["loss"] - scores[0]["loss"]) for row in scores[1:])
    index = math.ceil(.95 * len(differences)) - 1
    color_differences = sorted(abs(row["mean_color_mae_intersection"] - scores[0]["mean_color_mae_intersection"])
                               for row in scores[1:]) if all(isinstance(row.get("mean_color_mae_intersection"), (int, float)) for row in scores) else []
    return {"version": SCHEMA, "repeat_count": len(scores), "p95_paired_loss_difference": differences[index],
            "p95_paired_color_mae_difference": color_differences[index] if color_differences else None,
            "minimum_gain": max(.01, differences[index])}


def reference_from_run(run_dir: Path, output: Path | None) -> dict:
    """Build a version 1 reference from a baseline run: the run's own rig hash
    and, per view, its image and its audited mask. This is a self-reference --
    it pins the run's silhouette exactly, so scoring that same run must return
    IoU 1.0 for every view. It never authors a mask."""
    capture_record = load_json(run_dir / "capture.json")
    if capture_record.get("status") != "complete":
        raise EvaluationError("capture is incomplete; a reference cannot be built")
    rig_sha256 = capture_record.get("rig_sha256")
    if not isinstance(rig_sha256, str) or not re.fullmatch(r"[0-9a-f]{64}", rig_sha256):
        raise EvaluationError("capture run has no usable rig hash")
    rows = capture_record.get("views")
    if not isinstance(rows, dict) or not rows:
        raise EvaluationError("capture run has no views")
    views = {}
    for name in sorted(rows):
        row = rows[name]
        audited = audited_mask(run_dir, row, name)
        image_path = view_file(run_dir, f"{name}.png", f"capture image {name}")
        if file_hash(image_path) != row["image"]["sha256"]:
            raise EvaluationError(f"capture image changed: {name}")
        size = (row["image"]["width"], row["image"]["height"])
        mask_pixels(run_dir / f"{name}.mask.png", size)
        views[name] = {"image": image_path.name, "image_sha256": file_hash(image_path),
                       "reference_mask": f"{name}.mask.png",
                       "reference_mask_sha256": audited["sha256"],
                       "candidate_mask": f"{name}.mask.png"}
    destination = output if output is not None else run_dir / "reference.json"
    if destination.exists():
        raise EvaluationError(f"reference already exists; a baseline is never rewritten: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.parent.resolve() != run_dir.resolve():
        # The scorer resolves reference entries beside the reference JSON, so an
        # output elsewhere has to carry its own copy of every pinned PNG.
        for entry in views.values():
            for key in ("image", "reference_mask"):
                source, target = run_dir / entry[key], destination.parent / entry[key]
                if target.exists() and file_hash(target) != file_hash(source):
                    raise EvaluationError(f"refusing to overwrite a different file: {target}")
                shutil.copy2(source, target)
    reference = {"version": SCHEMA, "rig_sha256": rig_sha256, "views": views}
    destination.write_bytes(canonical(reference) + b"\n")
    return {"reference": str(destination), "rig_sha256": rig_sha256, "views": sorted(views)}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    p = sub.add_parser("freeze")
    p.add_argument("manifest", type=Path)
    p.add_argument("bundle", type=Path)
    p = sub.add_parser("capture")
    p.add_argument("bundle", type=Path)
    p.add_argument("run_dir", type=Path)
    p.add_argument("--editor", type=Path, default=ROOT / "MatterEditor/build/windows-msvc/editor.exe")
    p.add_argument("--timeout", type=float, default=120)
    p = sub.add_parser("reference-from-run")
    p.add_argument("run_dir", type=Path)
    p.add_argument("--output", type=Path)
    p = sub.add_parser("score")
    p.add_argument("run_dir", type=Path)
    p.add_argument("reference", type=Path)
    p = sub.add_parser("noise")
    p.add_argument("scores", nargs="+", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.action == "freeze":
            output = freeze(args.manifest, args.bundle)
        elif args.action == "capture":
            output = capture(args.bundle, args.run_dir, args.editor, args.timeout)
            if output["status"] != "complete":
                raise EvaluationError(output["error"])
        elif args.action == "reference-from-run":
            output = reference_from_run(args.run_dir, args.output)
        elif args.action == "score":
            output = score(args.run_dir, args.reference)
        else:
            output = noise(args.scores)
        print(json.dumps(output, indent=2))
        return 0
    except (EvaluationError, OSError, KeyError, ValueError, TimeoutError, subprocess.SubprocessError) as exc:
        print(f"object_eval: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
