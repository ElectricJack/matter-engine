#!/usr/bin/env python3
"""Send bounded, correlated requests through MatterEditor's command file.

The client never retries an appended request. A result-file reconnect only
continues polling for that request id, which is safe for edits as well as reads.
"""

from __future__ import annotations

import argparse
from collections.abc import Callable
import json
import os
from pathlib import Path
import re
import sys
import time
import uuid

PROTOCOL_VERSION = 1
MAX_REQUEST_BYTES = 64 * 1024
DEFAULT_TIMEOUT_SECONDS = 5.0
MAX_TIMEOUT_SECONDS = 30.0
MAX_BATCH_STEPS = 64
SESSION_NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")

EXIT_OK = 0
EXIT_COMMAND_FAILED = 1
EXIT_CLIENT_ERROR = 2
EXIT_BATCH_PARTIAL = 3


class ResultPoller:
    """Incrementally poll an append-only JSONL file, including replacement."""

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
            # A restarted editor may replace its result file. Drop old partial
            # bytes; this is a reconnect, never permission to resend work.
            self.offset = 0
            self.partial = b""
        if size == self.offset:
            return []
        try:
            with self.path.open("rb") as stream:
                stream.seek(self.offset)
                chunk = stream.read()
        except (FileNotFoundError, PermissionError, OSError):
            # Windows can briefly deny an open while creating/replacing a file.
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
    # One append + flush is the documented Windows polling transport.
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


def sessions_dir(value: Path | None) -> Path:
    return value or Path(os.environ.get("MATTER_AGENT_SESSIONS_DIR", ".matter-agent/sessions"))


def session_path(directory: Path, name: str) -> Path:
    if not SESSION_NAME_RE.fullmatch(name):
        raise ValueError("session name must be 1-64 ASCII letters, digits, '.', '_' or '-'")
    return directory / f"{name}.json"


def save_session(directory: Path, name: str, command_file: Path, result_file: Path) -> Path:
    path = session_path(directory, name)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {"version": 1, "cmd_file": str(command_file), "result_file": str(result_file)}
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)
    return path


def load_session(directory: Path, name: str) -> tuple[Path, Path]:
    path = session_path(directory, name)
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValueError(f"session {name!r} does not exist at {path}") from exc
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read session {name!r}: {exc}") from exc
    if not isinstance(value, dict) or value.get("version") != 1:
        raise ValueError(f"session {name!r} must be a version 1 object")
    command_file, result_file = value.get("cmd_file"), value.get("result_file")
    if not isinstance(command_file, str) or not command_file or not isinstance(result_file, str) or not result_file:
        raise ValueError(f"session {name!r} must contain non-empty cmd_file and result_file strings")
    return Path(command_file), Path(result_file)


def resolve_target(options: argparse.Namespace) -> tuple[Path, Path]:
    if options.session:
        if options.cmd_file or options.result_file:
            raise ValueError("--session cannot be combined with --cmd-file or --result-file")
        return load_session(sessions_dir(options.sessions_dir), options.session)
    if not options.cmd_file or not options.result_file:
        raise ValueError("provide both --cmd-file and --result-file, or target a named --session")
    return options.cmd_file, options.result_file


def validate_request_id(request_id: object, flag: str = "request_id") -> str:
    if not isinstance(request_id, str) or not request_id or len(request_id.encode("utf-8")) > 128 or any(
        ord(char) < 0x20 or ord(char) == 0x7F for char in request_id
    ):
        raise ValueError(f"{flag} must be a non-empty control-free string of at most 128 bytes")
    return request_id


def submit_request(command_file: Path, result_file: Path, envelope: dict[str, object], timeout_seconds: float) -> dict[str, object]:
    result_file.parent.mkdir(parents=True, exist_ok=True)
    result_file.touch(exist_ok=True)
    poller = ResultPoller(result_file)
    # Deliberately append exactly once. On a timeout, callers must inspect the
    # editor/session; automatic resends could duplicate destructive work.
    append_request(command_file, envelope)
    return wait_for_result(poller, str(envelope["request_id"]), timeout_seconds + 1.0)


def make_envelope(command: object, arguments: object, expected: object,
                  request_id: object, timeout_ms: object) -> dict[str, object]:
    if not isinstance(command, str) or not command:
        raise ValueError("command must be a non-empty string")
    if not isinstance(arguments, dict):
        raise ValueError("args must be a JSON object")
    if not isinstance(expected, dict):
        raise ValueError("expect must be a JSON object")
    if not isinstance(timeout_ms, int) or isinstance(timeout_ms, bool) or not 0 < timeout_ms <= int(MAX_TIMEOUT_SECONDS * 1000):
        raise ValueError("timeout_ms must be an integer from 1 through 30000")
    return {"version": PROTOCOL_VERSION, "request_id": validate_request_id(request_id),
            "command": command, "args": arguments, "timeout_ms": timeout_ms,
            **({"expect": expected} if expected else {})}


def load_batch(path: Path, default_timeout_seconds: float) -> tuple[str, bool, list[dict[str, object]]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read batch {path}: {exc}") from exc
    if not isinstance(document, dict) or document.get("version") != 1:
        raise ValueError("batch must be a version 1 JSON object")
    batch_id = validate_request_id(document.get("batch_id", str(uuid.uuid4())), "batch_id")
    stop_on_error = document.get("stop_on_error", True)
    if not isinstance(stop_on_error, bool):
        raise ValueError("batch stop_on_error must be boolean")
    steps = document.get("steps")
    if not isinstance(steps, list) or not steps or len(steps) > MAX_BATCH_STEPS:
        raise ValueError(f"batch steps must contain from 1 through {MAX_BATCH_STEPS} objects")
    default_timeout_ms = max(1, int(default_timeout_seconds * 1000))
    envelopes: list[dict[str, object]] = []
    seen_ids: set[str] = set()
    for index, step in enumerate(steps):
        if not isinstance(step, dict):
            raise ValueError(f"batch step {index} must be an object")
        request_id = validate_request_id(step.get("request_id", f"{batch_id}:{index + 1}"),
                                         f"batch step {index} request_id")
        if request_id in seen_ids:
            raise ValueError(f"batch step {index} repeats request_id {request_id!r}")
        seen_ids.add(request_id)
        envelopes.append(make_envelope(step.get("command"), step.get("args", {}),
                                       step.get("expect", {}), request_id,
                                       step.get("timeout_ms", default_timeout_ms)))
    return batch_id, stop_on_error, envelopes


def execute_batch(batch_id: str, stop_on_error: bool, envelopes: list[dict[str, object]],
                  command_file: Path, result_file: Path,
                  send: Callable[[Path, Path, dict[str, object], float], dict[str, object]] = submit_request) -> tuple[dict[str, object], int]:
    results: list[dict[str, object]] = []
    stopped = False
    client_error = False
    for index, envelope in enumerate(envelopes):
        try:
            terminal = send(command_file, result_file, envelope, int(envelope["timeout_ms"]) / 1000.0)
            row: dict[str, object] = {"index": index, "request": envelope, "result": terminal}
            failed = terminal.get("ok") is not True
        except (OSError, ValueError, TimeoutError) as exc:
            row = {"index": index, "request": envelope,
                   "client_error": {"code": "client_timeout", "message": str(exc)}}
            failed = True
            client_error = True
        results.append(row)
        if failed and stop_on_error:
            stopped = True
            break
    completed = len(results)
    payload = {"protocol": "matter-agent", "version": 1, "type": "batch_result",
               "batch_id": batch_id, "status": "partial" if stopped else "completed",
               "stop_on_error": stop_on_error, "transactional": False,
               "requested_steps": len(envelopes), "completed_steps": completed,
               "remaining_steps": len(envelopes) - completed, "steps": results}
    if client_error:
        return payload, EXIT_CLIENT_ERROR
    if stopped:
        return payload, EXIT_BATCH_PARTIAL
    return payload, EXIT_OK if all(row["result"].get("ok") is True for row in results) else EXIT_COMMAND_FAILED


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="send Matter Editor agent requests and JSON batches")
    parser.add_argument("command", nargs="?", help="command name (start with agent.commands)")
    parser.add_argument("--cmd-file", type=Path, help="same path as editor MATTER_CMD_FIFO")
    parser.add_argument("--result-file", type=Path, help="same path as editor MATTER_AGENT_RESULT_FILE")
    parser.add_argument("--session", help="named persistent target made with 'session set'")
    parser.add_argument("--sessions-dir", type=Path, help="target directory (default $MATTER_AGENT_SESSIONS_DIR or .matter-agent/sessions)")
    parser.add_argument("--request-id", default=None, help="lossless string id (default: random UUID)")
    parser.add_argument("--args", default="{}", dest="arguments", help="command arguments as a JSON object")
    parser.add_argument("--expect", default="{}", help="expected revisions as decimal-string JSON fields")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS, help="editor/client wait in seconds (max 30)")
    parser.add_argument("--batch", type=Path, help="version 1 JSON batch; commands run sequentially")
    parser.add_argument("--pretty", action="store_true", help="pretty-print JSON stdout")
    return parser


def session_main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="create or inspect Matter agent persistent targets")
    parser.add_argument("action", choices=("set", "show"))
    parser.add_argument("name")
    parser.add_argument("--sessions-dir", type=Path)
    parser.add_argument("--cmd-file", type=Path)
    parser.add_argument("--result-file", type=Path)
    options = parser.parse_args(argv)
    try:
        directory = sessions_dir(options.sessions_dir)
        if options.action == "set":
            if not options.cmd_file or not options.result_file:
                parser.error("session set requires --cmd-file and --result-file")
            path = save_session(directory, options.name, options.cmd_file, options.result_file)
            output = {"session": options.name, "path": str(path), "cmd_file": str(options.cmd_file),
                      "result_file": str(options.result_file)}
        else:
            command_file, result_file = load_session(directory, options.name)
            output = {"session": options.name, "cmd_file": str(command_file), "result_file": str(result_file)}
    except ValueError as exc:
        parser.error(str(exc))
    json.dump(output, sys.stdout, separators=(",", ":"))
    sys.stdout.write("\n")
    return EXIT_OK


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "session":
        return session_main(argv[1:])
    parser = build_parser()
    options = parser.parse_args(argv)
    if not (0.0 < options.timeout <= MAX_TIMEOUT_SECONDS):
        parser.error("--timeout must be greater than 0 and at most 30")
    if bool(options.command) == bool(options.batch):
        parser.error("provide exactly one command or --batch")
    try:
        command_file, result_file = resolve_target(options)
        if options.batch:
            batch_id, stop_on_error, envelopes = load_batch(options.batch, options.timeout)
            output, exit_code = execute_batch(batch_id, stop_on_error, envelopes, command_file, result_file)
        else:
            envelope = make_envelope(options.command, object_argument(options.arguments, "--args"),
                                     object_argument(options.expect, "--expect"),
                                     options.request_id or str(uuid.uuid4()),
                                     max(1, int(options.timeout * 1000)))
            output = submit_request(command_file, result_file, envelope, options.timeout)
            exit_code = EXIT_OK if output.get("ok") is True else EXIT_COMMAND_FAILED
    except (OSError, ValueError, TimeoutError) as exc:
        print(f"matter-agent: {exc}", file=sys.stderr)
        return EXIT_CLIENT_ERROR
    json.dump(output, sys.stdout, ensure_ascii=False, indent=2 if options.pretty else None,
              separators=None if options.pretty else (",", ":"))
    sys.stdout.write("\n")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
