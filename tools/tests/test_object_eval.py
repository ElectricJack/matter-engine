import json
import os
from pathlib import Path
import struct
import sys
import threading

from PIL import Image
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import object_eval as evaluation

PLANE_LAYOUT = (("identity", "rg32_uint", 8), ("depth", "r32_float", 4),
                ("normal", "rgba16_float", 8), ("color", "rgba16_float", 8),
                ("albedo", "rgba8_unorm", 4), ("orm", "rgba16_float", 8))
RIG_SHA256 = "b" * 64


def write_json(path, value):
    path.write_text(json.dumps(value), encoding="utf-8")


def channel_receipt(path, width, height):
    planes, offset = [], 16
    for name, plane_format, bytes_per_pixel in PLANE_LAYOUT:
        size = width * height * bytes_per_pixel
        planes.append({"name": name, "format": plane_format, "offset_bytes": str(offset),
                       "size_bytes": str(size)})
        offset += size
    return {"available": True, "container": "MECAP001", "path": str(path).replace("\\", "/"),
            "width": width, "height": height, "planes": planes}


def write_bundle(path, width, height, covered):
    """A byte-exact MECAP001 bundle whose identity plane marks exactly the
    requested pixels and whose other planes are inert filler."""
    payload = bytearray(b"MECAP001" + struct.pack("<II", width, height))
    identity = bytearray()
    for value in covered:
        identity += struct.pack("<II", *((7, 3) if value else (0xFFFFFFFF, 0xFFFFFFFF)))
    payload += identity
    payload += bytes(width * height * (4 + 8 + 8 + 4 + 8))
    path.write_bytes(bytes(payload))
    return channel_receipt(path, width, height)


def rock_manifest(root, source, world):
    return {"version": 1, "world": "ObjectEvalRock", "expected_root_module": "Rock",
            "expected_root_params": {"seed": 2},
            "source_paths": [str(source.relative_to(root)), str(world.relative_to(root))],
            "model_write_paths": [str(source.relative_to(root))],
            "rig": {"projection": {"vertical_fov_radians": .785, "near_plane": .1, "far_plane": 100},
                    "hold_frames": 2, "views": [{"name": "front", "pose": [1, 2, 3, 0, 0, 0]}]}}


def frozen_candidate(tmp_path, monkeypatch):
    """A frozen single-view rock candidate and a stand-in editor executable."""
    monkeypatch.setattr(evaluation, "ROOT", tmp_path)
    source = tmp_path / "projects/world_demo/objects/terrain/Rock.js"
    source.parent.mkdir(parents=True)
    source.write_text("class Rock {}")
    world = tmp_path / "projects/world_demo/scenes/terrain/ObjectEvalRock/ObjectEvalRock.js"
    world.parent.mkdir(parents=True)
    world.write_text("class ObjectEvalRock {}")
    manifest = rock_manifest(tmp_path, source, world)
    manifest_path = tmp_path / "manifest.json"
    write_json(manifest_path, manifest)
    bundle = tmp_path / "bundle"
    evaluation.freeze(manifest_path, bundle)
    editor_exe = tmp_path / "editor.exe"
    editor_exe.write_bytes(b"editor")
    monkeypatch.setattr(evaluation, "native_path", lambda path: str(path).replace("\\", "/"))
    return manifest, bundle, editor_exe


class FakeEditor:
    """A native editor that answers the capture protocol from a scripted rig and
    writes the capture, sidecar and channel bundle the real one would. The
    image and the identity plane are separately scriptable so a test can make
    them disagree."""

    def __init__(self, run_dir, world, executable, manifest, width, height, covered,
                 identity_covered=None):
        self.run_dir, self.world, self.manifest = run_dir, world, manifest
        self.width, self.height, self.covered = width, height, covered
        self.identity_covered = covered if identity_covered is None else identity_covered
        self.lines, self.lock = [], threading.Lock()

    def await_line(self, *args):
        pass

    def send_fifo(self, lines):
        pass

    def close(self):
        pass

    def request(self, command, args, timeout=30, accepted_codes=("ok",)):
        pose = self.manifest["rig"]["views"][0]["pose"]
        if command == "job.start":
            return {"result": {"job": {"job_id": "1"}}}
        if command == "job.wait":
            return {"result": {"job": {"state": "completed", "inputs": {"world": self.world}}}}
        if command == "scene.list_objects":
            return {"result": {"objects": [{"provenance": {
                "module": "Rock",
                "params_json": {"available": True,
                                "value": json.dumps(self.manifest["expected_root_params"])}}}],
                "page": {"total_matched": 1}}}
        if command != "viewport.capture":
            return {"result": {}}
        path = Path(args["path"].replace("/", os.sep))
        image = Image.new("RGB", (self.width, self.height), "black")
        for index, value in enumerate(self.covered):
            if value:
                image.putpixel((index % self.width, index // self.width), (200, 200, 200))
        image.save(path)
        Path(str(path) + ".done").write_text("captured\n")
        channels = write_bundle(Path(str(path) + ".channels.bin"), self.width, self.height,
                                self.identity_covered)
        return {"result": {"captured": {"scene": {"ready": True}, "frame": {"id": 7}},
                           "viewport": {"production_view": True}, "presented": {"id": 7},
                           "readiness": {"ready": True, "blockers": []},
                           "camera": {"position": pose[:3], "target": pose[3:], "up": [0, 1, 0],
                                      **self.manifest["rig"]["projection"]},
                           "image": {"width": self.width, "height": self.height},
                           "channels": channels}}


def test_freeze_scope_and_live_source_hash(tmp_path, monkeypatch):
    monkeypatch.setattr(evaluation, "ROOT", tmp_path)
    source = tmp_path / "projects/world_demo/objects/terrain/Rock.js"
    source.parent.mkdir(parents=True)
    source.write_text("class Rock {}")
    world = tmp_path / "projects/world_demo/scenes/terrain/ObjectEvalRock/ObjectEvalRock.js"
    world.parent.mkdir(parents=True)
    world.write_text("class ObjectEvalRock {}")
    manifest = {"version": 1, "world": "ObjectEvalRock", "expected_root_module": "Rock", "expected_root_params": {}, "source_paths": [str(source.relative_to(tmp_path)), str(world.relative_to(tmp_path))],
                "model_write_paths": [str(source.relative_to(tmp_path))],
                "rig": {"projection": {"vertical_fov_radians": .785, "near_plane": .1, "far_plane": 100},
                        "hold_frames": 2, "views": [{"name": "front", "pose": [1, 2, 3, 0, 0, 0]}]}}
    manifest_path = tmp_path / "manifest.json"
    write_json(manifest_path, manifest)
    bundle = tmp_path / "bundle"
    evaluation.freeze(manifest_path, bundle)
    assert evaluation.verify_bundle(bundle)["candidate_sha256"]
    source.write_text("invalid JS")
    with pytest.raises(evaluation.EvaluationError, match="live source closure"):
        evaluation.verify_bundle(bundle)
    manifest["model_write_paths"] = [str(world.relative_to(tmp_path))]
    write_json(manifest_path, manifest)
    with pytest.raises(evaluation.EvaluationError, match="outside generator"):
        evaluation.freeze(manifest_path, tmp_path / "rejected")
    manifest["model_write_paths"] = [str(source.relative_to(tmp_path))]
    write_json(manifest_path, manifest)
    with pytest.raises(evaluation.EvaluationError, match="invalid JS"):
        evaluation.freeze(manifest_path, tmp_path / "invalid-js")
    assert not (tmp_path / "invalid-js").exists()


def test_failed_reload_writes_failed_receipt(tmp_path, monkeypatch):
    monkeypatch.setattr(evaluation, "ROOT", tmp_path)
    source = tmp_path / "projects/world_demo/objects/terrain/Rock.js"
    source.parent.mkdir(parents=True)
    source.write_text("class Rock {}")
    manifest = {"version": 1, "world": "ObjectEvalRock", "expected_root_module": "Rock", "expected_root_params": {}, "source_paths": [str(source.relative_to(tmp_path))],
                "model_write_paths": [str(source.relative_to(tmp_path))],
                "rig": {"projection": {"vertical_fov_radians": .785, "near_plane": .1, "far_plane": 100},
                        "hold_frames": 2, "views": [{"name": "front", "pose": [1, 2, 3, 0, 0, 0]}]}}
    manifest_path = tmp_path / "manifest.json"
    write_json(manifest_path, manifest)
    bundle = tmp_path / "bundle"
    evaluation.freeze(manifest_path, bundle)
    editor_exe = tmp_path / "editor.exe"
    editor_exe.write_bytes(b"editor")

    class FailedEditor:
        def __init__(self, *args):
            pass
        def await_line(self, *args):
            pass
        def request(self, command, *args, **kwargs):
            if command == "job.start":
                return {"result": {"job": {"job_id": "1"}}}
            raise evaluation.EvaluationError("job.wait: execution_failure: script_error")
        def close(self):
            pass

    monkeypatch.setattr(evaluation, "Editor", FailedEditor)
    run = tmp_path / "run"
    result = evaluation.capture(bundle, run, editor_exe, 10)
    assert result["status"] == "failed" and "script_error" in result["error"]
    assert json.loads((run / "capture.json").read_text())["status"] == "failed"


def test_png_rejects_stale_or_corrupt(tmp_path):
    path = tmp_path / "front.png"
    Image.new("RGB", (32, 32), "red").save(path)
    with pytest.raises(evaluation.EvaluationError, match="completion marker"):
        evaluation.check_png(path, {"image": {"width": 32, "height": 32}})
    Path(str(path) + ".done").write_text("captured\n")
    with pytest.raises(evaluation.EvaluationError, match="empty foreground"):
        evaluation.check_png(path, {"image": {"width": 32, "height": 32}})
    path.write_bytes(b"bad png")
    with pytest.raises(Exception):
        evaluation.check_png(path, {"image": {"width": 32, "height": 32}})


def test_numeric_channel_bundle_must_match_receipt(tmp_path, monkeypatch):
    monkeypatch.setattr(evaluation, "native_path", lambda _: "C:/front.png.channels.bin")
    path = tmp_path / "front.png.channels.bin"
    path.write_bytes(b"MECAP001" + (2).to_bytes(4, "little") + (2).to_bytes(4, "little") + b"abcd")
    receipt = {"channels": {"available": True, "container": "MECAP001", "path": "C:/front.png.channels.bin",
                            "width": 2, "height": 2, "planes": [{"offset_bytes": str(16 + i), "size_bytes": "1"} for i in range(4)]}}
    with pytest.raises(evaluation.EvaluationError, match="incomplete"):
        evaluation.check_channels(path, receipt)


def audited_run(run_dir, width=32, height=32, covered=None, status="complete"):
    """A run directory holding a captured PNG, its audited mask and the receipt
    that pins both hashes."""
    run_dir.mkdir(parents=True, exist_ok=True)
    image = Image.new("RGB", (width, height), "black")
    image.putpixel((width // 2, height // 2), (255, 255, 255))
    image.save(run_dir / "front.png")
    if covered is None:
        covered = [(index % width - width // 2) ** 2 + (index // width - height // 2) ** 2 <= 36
                   for index in range(width * height)]
    mask = Image.new("L", (width, height), 0)
    mask.putdata([255 if value else 0 for value in covered])
    mask.save(run_dir / "front.mask.png")
    audit = evaluation.audit_mask(covered, (width, height), (width, height))
    audit.update(path=str(run_dir / "front.mask.png"), sha256=evaluation.file_hash(run_dir / "front.mask.png"))
    assert audit["audited"] is True
    write_json(run_dir / "capture.json", {"version": 1, "run_id": "test-run", "status": status,
                                           "candidate_sha256": "a", "rig_sha256": RIG_SHA256, "views": {"front": {
                                               "image": {"sha256": evaluation.file_hash(run_dir / "front.png"),
                                                         "width": width, "height": height},
                                               "mask_audited": audit}}})
    return run_dir


def test_candidate_mask_is_derived_from_the_identity_plane(tmp_path):
    width, height = 8, 4
    covered = [index in (0, 1, 9, 10, 11) for index in range(width * height)]
    bundle_path = tmp_path / "front.png.channels.bin"
    channels = write_bundle(bundle_path, width, height, covered)
    audit = evaluation.emit_candidate_mask(tmp_path, "front", bundle_path, channels, (width, height))
    assert audit["audited"] is True and audit["failures"] == []
    assert audit["object_pixels"] == 5 and audit["resampled_to_image"] is False
    assert audit["bounds"] == {"min_x": 0, "max_x": 3, "min_y": 0, "max_y": 1}
    written = evaluation.mask_pixels(tmp_path / "front.mask.png", (width, height))
    assert written == covered
    assert audit["sha256"] == evaluation.file_hash(tmp_path / "front.mask.png")


def test_candidate_mask_refuses_an_unusable_identity_plane(tmp_path):
    bundle_path = tmp_path / "front.png.channels.bin"
    channels = write_bundle(bundle_path, 4, 4, [False] * 16)
    with pytest.raises(evaluation.EvaluationError, match="identity plane"):
        evaluation.emit_candidate_mask(tmp_path, "front", bundle_path, {**channels, "planes": [
            {"name": "identity", "format": "rgba8_unorm", "offset_bytes": "16",
             "size_bytes": str(4 * 4 * 8)}] + channels["planes"][1:]}, (4, 4))
    with pytest.raises(evaluation.EvaluationError, match="channel grid"):
        evaluation.emit_candidate_mask(tmp_path, "front", bundle_path, {**channels, "planes": [
            {"name": "identity", "format": "rg32_uint", "offset_bytes": "16",
             "size_bytes": str(4 * 4 * 4)}] + channels["planes"][1:]}, (4, 4))
    truncated = tmp_path / "short.bin"
    truncated.write_bytes(bundle_path.read_bytes()[:16 + 4 * 4 * 8 - 8])
    with pytest.raises(evaluation.EvaluationError, match="truncated"):
        evaluation.emit_candidate_mask(tmp_path, "front", truncated, channel_receipt(truncated, 4, 4), (4, 4))
    evaluation.emit_candidate_mask(tmp_path, "front", bundle_path, channels, (4, 4))
    with pytest.raises(evaluation.EvaluationError, match="stale mask"):
        evaluation.emit_candidate_mask(tmp_path, "front", bundle_path, channels, (4, 4))


def test_mask_audit_refuses_empty_oversized_and_resampled_grids(tmp_path):
    empty = evaluation.audit_mask([False] * 64, (8, 8), (8, 8))
    assert empty["audited"] is False
    assert empty["failures"] == ["area_within_bounds", "inside_frame", "nonempty"]
    assert empty["object_pixels"] == 0 and empty["bounds"] is None
    full = evaluation.audit_mask([True] * 64, (8, 8), (8, 8))
    assert full["audited"] is False and full["failures"] == ["area_within_bounds"]
    assert full["coverage_fraction"] == 1.0
    lit = [index % 8 in (2, 3) and index // 8 in (2, 3) for index in range(64)]
    scaled = evaluation.audit_mask(lit, (8, 8), (4, 4))
    assert scaled["audited"] is False and scaled["failures"] == ["matches_channel_dimensions"]
    assert scaled["resampled_to_image"] is True
    with pytest.raises(evaluation.EvaluationError, match="exactly one frame"):
        evaluation.audit_mask([True] * 16, (8, 8), (8, 8))


def test_capture_records_an_audited_mask_per_view(tmp_path, monkeypatch):
    manifest, bundle, editor_exe = frozen_candidate(tmp_path, monkeypatch)
    width, height = 64, 48
    covered = [8 <= index % width <= 40 and 6 <= index // width <= 30 for index in range(width * height)]
    monkeypatch.setattr(evaluation, "Editor",
                        lambda *args: FakeEditor(*args, manifest, width, height, covered))
    run = tmp_path / "run"
    receipt = evaluation.capture(bundle, run, editor_exe, 10)
    assert receipt["status"] == "complete", receipt.get("error")
    audit = receipt["views"]["front"]["mask_audited"]
    assert audit["audited"] is True and audit["matches_channel_dimensions"] is True
    assert audit["object_pixels"] == sum(covered)
    assert audit["width"] == width and audit["height"] == height
    assert evaluation.mask_pixels(run / "front.mask.png", (width, height)) == covered
    assert json.loads((run / "capture.json").read_text())["views"]["front"]["mask_audited"]["audited"] is True
    assert evaluation.reference_from_run(run, None)["views"] == ["front"]


def test_capture_fails_closed_when_the_mask_audit_fails(tmp_path, monkeypatch):
    manifest, bundle, editor_exe = frozen_candidate(tmp_path, monkeypatch)
    width = height = 32
    lit = [abs(index % width - 16) + abs(index // width - 16) < 8 for index in range(width * height)]
    monkeypatch.setattr(evaluation, "Editor", lambda *args: FakeEditor(
        *args, manifest, width, height, lit, [False] * (width * height)))
    receipt = evaluation.capture(bundle, tmp_path / "run", editor_exe, 10)
    assert receipt["status"] == "failed" and "nonempty" in receipt["error"]
    assert receipt["views"]["front"]["mask_audited"]["audited"] is False
    with pytest.raises(evaluation.EvaluationError, match="incomplete"):
        evaluation.reference_from_run(tmp_path / "run", None)


def test_capture_fails_closed_when_the_raster_extent_differs_from_the_image(tmp_path, monkeypatch):
    manifest, bundle, editor_exe = frozen_candidate(tmp_path, monkeypatch)
    width, height = 64, 48
    covered = [8 <= index % width <= 40 and 6 <= index // width <= 30 for index in range(width * height)]

    class HalfResolutionEditor(FakeEditor):
        def request(self, command, args, timeout=30, accepted_codes=("ok",)):
            result = super().request(command, args, timeout, accepted_codes)
            if command == "viewport.capture":
                path = Path(args["path"].replace("/", os.sep))
                channels = write_bundle(Path(str(path) + ".channels.bin"), width // 2, height // 2,
                                        [value for index, value in enumerate(covered)
                                         if index % width < width // 2 and index // width < height // 2])
                result["result"]["channels"] = channels
            return result

    monkeypatch.setattr(evaluation, "Editor", lambda *args: HalfResolutionEditor(*args, manifest, width, height, covered))
    receipt = evaluation.capture(bundle, tmp_path / "run", editor_exe, 10)
    assert receipt["status"] == "failed" and "matches_channel_dimensions" in receipt["error"]
    audit = receipt["views"]["front"]["mask_audited"]
    assert audit["resampled_to_image"] is True and audit["audited"] is False
    assert (audit["channel_width"], audit["channel_height"]) == (width // 2, height // 2)
    assert (audit["width"], audit["height"]) == (width, height)


def test_reference_from_run_is_a_self_referential_baseline(tmp_path):
    run = audited_run(tmp_path / "run")
    result = evaluation.reference_from_run(run, None)
    assert result["views"] == ["front"] and result["reference"] == str(run / "reference.json")
    reference = json.loads((run / "reference.json").read_text())
    assert reference["version"] == 1 and reference["rig_sha256"] == RIG_SHA256
    entry = reference["views"]["front"]
    assert entry == {"image": "front.png", "image_sha256": evaluation.file_hash(run / "front.png"),
                     "reference_mask": "front.mask.png",
                     "reference_mask_sha256": evaluation.file_hash(run / "front.mask.png"),
                     "candidate_mask": "front.mask.png"}
    scored = evaluation.score(run, run / "reference.json")
    assert scored["worst_iou"] == 1 and scored["mean_iou"] == 1 and scored["loss"] == 0
    with pytest.raises(evaluation.EvaluationError, match="never rewritten"):
        evaluation.reference_from_run(run, None)


def test_reference_from_run_can_write_a_detached_baseline(tmp_path):
    run = audited_run(tmp_path / "run")
    detached = tmp_path / "reference" / "rock-reference-v1.json"
    evaluation.reference_from_run(run, detached)
    assert json.loads(detached.read_text())["rig_sha256"] == RIG_SHA256
    assert (detached.parent / "front.png").is_file() and (detached.parent / "front.mask.png").is_file()
    assert evaluation.score(run, detached)["loss"] == 0
    collided = tmp_path / "collided"
    collided.mkdir()
    (collided / "front.png").write_bytes(b"different")
    with pytest.raises(evaluation.EvaluationError, match="refusing to overwrite"):
        evaluation.reference_from_run(run, collided / "rock-reference-v1.json")


def test_reference_from_run_refuses_unaudited_or_changed_runs(tmp_path):
    run = audited_run(tmp_path / "run", status="failed")
    with pytest.raises(evaluation.EvaluationError, match="incomplete"):
        evaluation.reference_from_run(run, None)
    run = audited_run(tmp_path / "complete-run")
    record = json.loads((run / "capture.json").read_text())
    record["views"]["front"]["mask_audited"]["audited"] = False
    write_json(run / "capture.json", record)
    with pytest.raises(evaluation.EvaluationError, match="not audited"):
        evaluation.reference_from_run(run, None)
    run = audited_run(tmp_path / "mask-changed")
    Image.new("L", (32, 32), 255).save(run / "front.mask.png")
    with pytest.raises(evaluation.EvaluationError, match="missing or changed"):
        evaluation.reference_from_run(run, None)
    run = audited_run(tmp_path / "image-changed")
    Image.new("RGB", (32, 32), "red").save(run / "front.png")
    with pytest.raises(evaluation.EvaluationError, match="image changed"):
        evaluation.reference_from_run(run, None)
    run = audited_run(tmp_path / "no-rig")
    record = json.loads((run / "capture.json").read_text())
    record.pop("rig_sha256")
    write_json(run / "capture.json", record)
    with pytest.raises(evaluation.EvaluationError, match="rig hash"):
        evaluation.reference_from_run(run, None)


def test_score_requires_complete_coverage_and_audited_masks(tmp_path):
    run = audited_run(tmp_path / "run")
    reference_dir = tmp_path / "reference"
    reference_dir.mkdir()
    Image.open(run / "front.png").save(reference_dir / "front.png")
    Image.open(run / "front.mask.png").save(reference_dir / "front.mask.png")
    reference = {"version": 1, "rig_sha256": RIG_SHA256, "views": {"front": {
        "image": "front.png", "image_sha256": evaluation.file_hash(reference_dir / "front.png"),
        "reference_mask": "front.mask.png",
        "reference_mask_sha256": evaluation.file_hash(reference_dir / "front.mask.png"),
        "candidate_mask": "front.mask.png"}}}
    reference_path = reference_dir / "reference.json"
    write_json(reference_path, reference)
    record = json.loads((run / "capture.json").read_text())
    record["status"] = "failed"
    write_json(run / "capture.json", record)
    with pytest.raises(evaluation.EvaluationError, match="incomplete"):
        evaluation.score(run, reference_path)
    record["status"] = "complete"
    write_json(run / "capture.json", record)
    result = evaluation.score(run, reference_path)
    assert result["worst_iou"] == 1 and result["loss"] == 0
    reference["views"]["back"] = reference["views"]["front"]
    write_json(reference_path, reference)
    with pytest.raises(evaluation.EvaluationError, match="coverage"):
        evaluation.score(run, reference_path)
    reference["views"].pop("back")
    write_json(reference_path, reference)
    mask = Image.open(run / "front.mask.png")
    mask.putpixel((0, 0), 128)
    mask.save(run / "front.mask.png")
    with pytest.raises(evaluation.EvaluationError, match="missing or changed"):
        evaluation.score(run, reference_path)


def test_score_refuses_an_unaudited_or_foreign_candidate_mask(tmp_path):
    run = audited_run(tmp_path / "run")
    reference_dir = tmp_path / "reference"
    reference_dir.mkdir()
    Image.open(run / "front.png").save(reference_dir / "front.png")
    Image.open(run / "front.mask.png").save(reference_dir / "front.mask.png")
    Image.open(run / "front.mask.png").save(reference_dir / "other.png")
    Image.open(run / "front.mask.png").save(run / "other.png")
    reference = {"version": 1, "rig_sha256": RIG_SHA256, "views": {"front": {
        "image": "front.png", "image_sha256": evaluation.file_hash(reference_dir / "front.png"),
        "reference_mask": "front.mask.png",
        "reference_mask_sha256": evaluation.file_hash(reference_dir / "front.mask.png"),
        "candidate_mask": "other.png"}}}
    reference_path = reference_dir / "reference.json"
    write_json(reference_path, reference)
    with pytest.raises(evaluation.EvaluationError, match="not this run's audited mask"):
        evaluation.score(run, reference_path)
    reference["views"]["front"]["candidate_mask"] = "../run/front.mask.png"
    write_json(reference_path, reference)
    with pytest.raises(evaluation.EvaluationError, match="plain PNG filename"):
        evaluation.score(run, reference_path)
    reference["views"]["front"]["candidate_mask"] = "front.mask.png"
    reference["views"]["front"]["image"] = "../run/front.png"
    write_json(reference_path, reference)
    with pytest.raises(evaluation.EvaluationError, match="plain PNG filename"):
        evaluation.score(run, reference_path)
    grey = Image.open(reference_dir / "front.mask.png")
    grey.putpixel((0, 0), 128)
    grey.save(reference_dir / "front.mask.png")
    reference["views"]["front"]["image"] = "front.png"
    reference["views"]["front"]["reference_mask_sha256"] = evaluation.file_hash(reference_dir / "front.mask.png")
    write_json(reference_path, reference)
    with pytest.raises(evaluation.EvaluationError, match="binary"):
        evaluation.score(run, reference_path)


def test_noise_requires_five_identical_runs(tmp_path):
    paths = []
    for i, loss in enumerate([.2, .21, .205, .195, .2]):
        path = tmp_path / f"{i}.json"
        write_json(path, {"version": 1, "run_id": f"run-{i}", "status": "complete", "candidate_sha256": "a",
                          "rig_sha256": "b", "reference_sha256": "c", "loss": loss})
        paths.append(path)
    assert evaluation.noise(paths)["minimum_gain"] == pytest.approx(.01)
    with pytest.raises(evaluation.EvaluationError, match="at least five"):
        evaluation.noise(paths[:4])
    with pytest.raises(evaluation.EvaluationError, match="distinct runs"):
        evaluation.noise(paths[:4] + paths[:1])
    row = json.loads(paths[4].read_text())
    row["run_id"] = "run-0"
    write_json(paths[4], row)
    with pytest.raises(evaluation.EvaluationError, match="distinct capture run IDs"):
        evaluation.noise(paths)
    row["run_id"] = "run-4"
    write_json(paths[4], row)
    for path in paths:
        row = json.loads(path.read_text())
        row["loss"] = 0
        write_json(path, row)
    assert evaluation.noise(paths)["minimum_gain"] == .01
