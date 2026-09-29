import hashlib
import json
from pathlib import Path
import sys

from PIL import Image
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import object_eval as evaluation


def write_json(path, value):
    path.write_text(json.dumps(value), encoding="utf-8")


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


def test_score_requires_complete_coverage_and_audited_masks(tmp_path):
    run = tmp_path / "run"
    run.mkdir()
    reference_dir = tmp_path / "reference"
    reference_dir.mkdir()
    image = Image.new("RGB", (32, 32), "black")
    image.putpixel((16, 16), (255, 255, 255))
    image.save(run / "front.png")
    image.save(reference_dir / "front.png")
    mask = Image.new("L", (32, 32), 0)
    mask.putpixel((16, 16), 255)
    mask.save(run / "mask.png")
    mask.save(reference_dir / "mask.png")
    capture = {"version": 1, "run_id": "test-run", "status": "failed", "candidate_sha256": "a", "rig_sha256": "b",
               "views": {"front": {"image": {"sha256": evaluation.file_hash(run / "front.png")}}}}
    write_json(run / "capture.json", capture)
    reference = {"version": 1, "rig_sha256": "b", "views": {"front": {
        "image": "front.png", "image_sha256": evaluation.file_hash(reference_dir / "front.png"),
        "reference_mask": "mask.png", "reference_mask_sha256": evaluation.file_hash(reference_dir / "mask.png"),
        "candidate_mask": "mask.png"}}}
    reference_path = reference_dir / "reference.json"
    write_json(reference_path, reference)
    with pytest.raises(evaluation.EvaluationError, match="incomplete"):
        evaluation.score(run, reference_path)
    capture["status"] = "complete"
    write_json(run / "capture.json", capture)
    result = evaluation.score(run, reference_path)
    assert result["worst_iou"] == 1 and result["loss"] == 0
    reference["views"]["back"] = reference["views"]["front"]
    write_json(reference_path, reference)
    with pytest.raises(evaluation.EvaluationError, match="coverage"):
        evaluation.score(run, reference_path)
    reference["views"].pop("back")
    write_json(reference_path, reference)
    mask.putpixel((0, 0), 128)
    mask.save(run / "mask.png")
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
