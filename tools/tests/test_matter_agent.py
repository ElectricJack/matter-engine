#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "matter_agent", ROOT / "tools" / "matter_agent.py"
)
assert SPEC and SPEC.loader
matter_agent = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(matter_agent)


def response(request_id: str) -> bytes:
    return (
        json.dumps(
            {
                "protocol": "matter-agent",
                "version": 1,
                "type": "result",
                "request_id": request_id,
                "ticket_id": "9007199254740993",
                "ok": True,
                "code": "ok",
            },
            separators=(",", ":"),
        )
        + "\n"
    ).encode()


class MatterAgentClientTests(unittest.TestCase):
    def test_append_escaping_and_stdout_safe_envelope(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            command_file = Path(directory) / "commands.txt"
            envelope = {
                "version": 1,
                "request_id": 'quote-"-\\-snowman-☃',
                "command": "agent.help",
                "args": {"command": "agent.commands"},
            }
            matter_agent.append_request(command_file, envelope)
            raw = command_file.read_text(encoding="utf-8")
            self.assertTrue(raw.startswith("agent "))
            self.assertEqual(json.loads(raw[6:]), envelope)

    def test_partial_mixed_logs_and_delayed_completion(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            result_file = Path(directory) / "results.jsonl"
            result_file.write_bytes(b"old terminal\n")
            poller = matter_agent.ResultPoller(result_file)

            def writer() -> None:
                time.sleep(0.03)
                with result_file.open("ab") as stream:
                    stream.write(b"human log accidentally mixed in\n")
                    record = response("delayed")
                    stream.write(record[:17])
                    stream.flush()
                    time.sleep(0.03)
                    stream.write(record[17:])
                    stream.flush()

            thread = threading.Thread(target=writer)
            thread.start()
            result = matter_agent.wait_for_result(
                poller, "delayed", timeout_seconds=1.0, poll_interval=0.005
            )
            thread.join()
            self.assertEqual(result["ticket_id"], "9007199254740993")

    def test_windows_style_file_reconnect_after_truncate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            result_file = Path(directory) / "results.jsonl"
            result_file.write_bytes(b"x" * 200)
            poller = matter_agent.ResultPoller(result_file)
            result_file.write_bytes(response("reconnected"))
            records = poller.poll()
            self.assertEqual(records[0]["request_id"], "reconnected")

    def test_client_timeout_is_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            result_file = Path(directory) / "results.jsonl"
            result_file.touch()
            start = time.monotonic()
            with self.assertRaises(TimeoutError):
                matter_agent.wait_for_result(
                    matter_agent.ResultPoller(result_file), "never", 0.03, 0.005
                )
            self.assertLess(time.monotonic() - start, 0.5)

    def test_named_session_round_trips_without_cwd_state(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sessions = Path(directory) / "sessions"
            command_file = Path("C:/tmp/matter/commands.txt")
            result_file = Path("C:/tmp/matter/results.jsonl")
            written = matter_agent.save_session(
                sessions, "physics-playground", command_file, result_file
            )
            self.assertEqual(written, sessions / "physics-playground.json")
            self.assertEqual(
                matter_agent.load_session(sessions, "physics-playground"),
                (command_file, result_file),
            )

    def test_batch_stops_in_order_and_reports_partial_completion(self) -> None:
        envelopes = [
            matter_agent.make_envelope("scene.list_objects", {}, {}, "batch:1", 5000),
            matter_agent.make_envelope("procedural.update", {}, {}, "batch:2", 5000),
            matter_agent.make_envelope("viewport.capture", {}, {}, "batch:3", 5000),
        ]
        sent: list[str] = []

        def send(_cmd: Path, _result: Path, envelope: dict[str, object], _timeout: float) -> dict[str, object]:
            sent.append(str(envelope["request_id"]))
            return {"ok": envelope["request_id"] != "batch:2", "code": "ok"}

        output, exit_code = matter_agent.execute_batch(
            "batch", True, envelopes, Path("commands.txt"), Path("results.jsonl"), send
        )
        self.assertEqual(sent, ["batch:1", "batch:2"])
        self.assertEqual(exit_code, matter_agent.EXIT_BATCH_PARTIAL)
        self.assertEqual(output["status"], "partial")
        self.assertEqual(output["completed_steps"], 2)
        self.assertEqual(output["remaining_steps"], 1)
        self.assertFalse(output["transactional"])

    def test_batch_continue_on_error_keeps_each_terminal_result(self) -> None:
        envelopes = [
            matter_agent.make_envelope("scene.list_objects", {}, {}, "batch:1", 5000),
            matter_agent.make_envelope("viewport.capture", {}, {}, "batch:2", 5000),
        ]

        def send(_cmd: Path, _result: Path, envelope: dict[str, object], _timeout: float) -> dict[str, object]:
            return {"ok": envelope["request_id"] == "batch:2", "code": "ok"}

        output, exit_code = matter_agent.execute_batch(
            "batch", False, envelopes, Path("commands.txt"), Path("results.jsonl"), send
        )
        self.assertEqual(exit_code, matter_agent.EXIT_COMMAND_FAILED)
        self.assertEqual(output["status"], "completed")
        self.assertEqual(output["completed_steps"], 2)
        self.assertEqual(len(output["steps"]), 2)

    def test_batch_ids_are_stable_and_duplicates_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            batch = Path(directory) / "batch.json"
            batch.write_text(json.dumps({
                "version": 1,
                "batch_id": "daily-inspect",
                "steps": [{"command": "scene.list_objects"}, {"command": "job.list"}],
            }), encoding="utf-8")
            batch_id, stop_on_error, envelopes = matter_agent.load_batch(batch, 5.0)
            self.assertEqual(batch_id, "daily-inspect")
            self.assertTrue(stop_on_error)
            self.assertEqual([item["request_id"] for item in envelopes],
                             ["daily-inspect:1", "daily-inspect:2"])

            batch.write_text(json.dumps({
                "version": 1,
                "batch_id": "duplicate",
                "steps": [
                    {"command": "scene.list_objects", "request_id": "same"},
                    {"command": "job.list", "request_id": "same"},
                ],
            }), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "repeats request_id"):
                matter_agent.load_batch(batch, 5.0)


if __name__ == "__main__":
    unittest.main()
