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


if __name__ == "__main__":
    unittest.main()
