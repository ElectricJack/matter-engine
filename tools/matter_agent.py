#!/usr/bin/env python3
"""Send one versioned request through MatterEditor's existing command file.

Only the terminal JSON object is written to stdout. Diagnostics and malformed
foreign lines go to stderr, keeping stdout safe for jq/PowerShell parsing.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import time
import uuid

PROTOCOL_VERSION = 1
MAX_REQUEST_BYTES = 64 * 1024
DEFAULT_TIMEOUT_SECONDS = 5.0
MAX_TIMEOUT_SECONDS = 30.0


class ResultPoller:
    """Incrementally polls an append-only JSONL file, including truncation."""

    def __init__(self, path: Path, offset: int | None = None) -> None:
        self.path = path
        self.offset = path.stat().st_size if offset is None and path.exists() else (offset or 0)
        self.partial = b""

    def poll(self) -> list[dict[str, object]]:
        try:
            size = self.path.stat().st_size
        except FileNotFoundError:
            return []
        if size < self.offset:
            # The editor/result file was replaced. Never join old partial bytes
            # to the new generation.
            self.offset = 0
            self.partial = b""
        if size == self.offset:
            return []
        try:
            with self.path.open("rb") as stream:
                stream.seek(self.offset)
                chunk = stream.read()
        except (FileNotFoundError, PermissionError, OSError):
            # Windows can transiently deny an open while another process is
            # creating/replacing the file. Poll again until the client deadline.
            return []
        self.offset += len(chunk)
        self.partial += chunk
        records: list[dict[str, object]] = []
        while b"\n" in self.partial:
            raw, self.partial = self.partial.split(b"\n", 1)
            if not raw.strip():
                continue
            try:
                value = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                print(f"matter-agent: ignored non-JSON result line: {exc}", file=sys.stderr)
                continue
            if not isinstance(value, dict):
                print("matter-agent: ignored non-object result line", file=sys.stderr)
                continue
            records.append(value)
        return records


def append_request(path: Path, envelope: dict[str, object]) -> None:
    encoded = ("agent " + json.dumps(envelope, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")
    if len(encoded) > MAX_REQUEST_BYTES:
        raise ValueError(f"encoded request exceeds {MAX_REQUEST_BYTES} bytes")
    path.parent.mkdir(parents=True, exist_ok=True)
    # One append + flush is the documented Windows polling transport. The
    # editor still accepts partial writes from other clients; this client has no
    # reason to manufacture one.
    with path.open("ab") as stream:
        stream.write(encoded)
        stream.flush()
        os.fsync(stream.fileno())


def wait_for_result(
    poller: ResultPoller, request_id: str, timeout_seconds: float,
    poll_interval: float = 0.02,
) -> dict[str, object]:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        for record in poller.poll():
            if (
                record.get("protocol") == "matter-agent"
                and record.get("version") == PROTOCOL_VERSION
                and record.get("type") == "result"
                and record.get("request_id") == request_id
            ):
                return record
        time.sleep(poll_interval)
    raise TimeoutError(f"no terminal result for request_id {request_id!r}")


def object_argument(text: str, flag: str) -> dict[str, object]:
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ValueError(f"{flag} is not valid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{flag} must be a JSON object")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="send one Matter Editor agent request and print one JSON result"
    )
    parser.add_argument("command", help="command name (start with agent.commands)")
    parser.add_argument("--cmd-file", required=True, type=Path,
                        help="same path as editor MATTER_CMD_FIFO")
    parser.add_argument("--result-file", required=True, type=Path,
                        help="same path as editor MATTER_AGENT_RESULT_FILE")
    parser.add_argument("--request-id", default=None,
                        help="lossless string id (default: random UUID)")
    parser.add_argument("--args", default="{}", dest="arguments",
                        help="command arguments as a JSON object")
    parser.add_argument("--expect", default="{}",
                        help="expected revisions as decimal-string JSON fields")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS,
                        help="editor and client wait in seconds (max 30)")
    parser.add_argument("--pretty", action="store_true",
                        help="pretty-print the one terminal JSON object")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    options = parser.parse_args(argv)
    if not (0.0 < options.timeout <= MAX_TIMEOUT_SECONDS):
        parser.error("--timeout must be greater than 0 and at most 30")
    try:
        arguments = object_argument(options.arguments, "--args")
        expected = object_argument(options.expect, "--expect")
    except ValueError as exc:
        parser.error(str(exc))
    request_id = options.request_id or str(uuid.uuid4())
    if not request_id or len(request_id.encode("utf-8")) > 128 or any(
        ord(char) < 0x20 or ord(char) == 0x7F for char in request_id
    ):
        parser.error("--request-id must be a non-empty control-free string of at most 128 bytes")

    options.result_file.parent.mkdir(parents=True, exist_ok=True)
    options.result_file.touch(exist_ok=True)
    poller = ResultPoller(options.result_file)
    envelope: dict[str, object] = {
        "version": PROTOCOL_VERSION,
        "request_id": request_id,
        "command": options.command,
        "args": arguments,
        "timeout_ms": max(1, int(options.timeout * 1000)),
    }
    if expected:
        envelope["expect"] = expected
    try:
        append_request(options.cmd_file, envelope)
        result = wait_for_result(poller, request_id, options.timeout + 1.0)
    except (OSError, ValueError, TimeoutError) as exc:
        print(f"matter-agent: {exc}", file=sys.stderr)
        return 2

    json.dump(result, sys.stdout, ensure_ascii=False,
              indent=2 if options.pretty else None,
              separators=None if options.pretty else (",", ":"))
    sys.stdout.write("\n")
    return 0 if result.get("ok") is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
