#!/usr/bin/env python3
"""drive.py -- launch MatterEditor against a QA timeline (a MATTER_CMD_FIFO
command file) and verify the screenshots it was supposed to produce.

Usage:
    drive.py --world CornellBox --timeline shots.txt --out-dir out/ \
        [--timeout 600] [--editor <path>] [--hide-ui] [--env K=V ...]

What it does:
  1. Creates --out-dir and copies --timeline into <out-dir>/cmd.txt -- the
     file the editor's MATTER_CMD_FIFO poller reads (main.cpp polls it as
     an append-only file on Windows; the whole timeline already being
     present when the editor starts is fine, it is read from offset 0).
  2. Scans the timeline for `shot`/`shot_now` lines and UNLINKS each
     screenshot's PNG and `.done` sidecar if either already exists, before
     the editor is ever launched. Without this, a run that crashes/hangs
     before actually producing a shot can false-pass by reading stale
     files a PREVIOUS run into the same --out-dir left behind -- the
     post-run check below only ever confirms the files exist, not that
     THIS run wrote them. Prints how many stale files it removed.
  3. Warns (to stderr, non-fatal) if the timeline has no `quit` line: such
     a timeline only ever ends via --timeout (or being closed some other
     way), which is sometimes intentional but is also the most common way
     a broken timeline silently burns the whole --timeout instead of
     failing fast.
  4. Launches editor.exe with cwd=MatterEditor/, MATTER_WORLD=<world>,
     MATTER_CMD_FIFO=<abs path to cmd.txt>, TMP/TEMP pointed at the OS
     temp dir, MATTER_HIDE_UI=1 if --hide-ui was passed, plus any --env
     K=V overrides applied last (so they can override any of the above).
  5. Tees the editor's stdout+stderr to <out-dir>/log.txt (and to this
     process's own stdout) while waiting for it to exit, up to --timeout
     seconds. On timeout, the process TREE is killed and drive.py exits 2.
  6. On a normal exit, checks that each shot/shot_now screenshot's PNG and
     its `.done` sidecar both exist (see step 2 -- these can only be from
     THIS run). Any missing file -> exit 1, with the missing paths listed.
     A nonzero editor exit code is also treated as failure (exit 1), even
     if every screenshot happened to be on disk -- a crash mid-script
     should never read as a pass. Otherwise: exit 0, with a one-line
     summary.

IMPORTANT: shot/shot_now paths in a timeline are resolved by the EDITOR
relative to its own working directory (MatterEditor/), NOT to --out-dir or
wherever drive.py itself was invoked from. This script resolves relative
shot paths the same way when verifying them. To avoid ambiguity, prefer
ABSOLUTE paths into --out-dir for every shot/shot_now line in the timeline
(that is what the acceptance timelines do).

Never launches two editors at once: this script only ever starts the one
process it owns and waits for it before returning, so nothing here can
race a second instance. It does not check for other, unrelated editor.exe
processes the user may have open.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

# MSYS2/Cygwin Python treats 'C:/...' as a RELATIVE path (is_absolute() is
# False on posix), silently mis-resolving every Windows path this script
# handles, and its os.name=='posix' skips the taskkill process-tree kill.
# Refuse it up front rather than fail confusingly later.
if sys.platform in ("cygwin", "msys"):
    sys.exit(
        "drive.py: MSYS2/Cygwin Python cannot handle Windows paths. "
        "Run with native Windows Python instead, e.g.: py -3 " + __file__
    )


def parse_args(argv):
    ap = argparse.ArgumentParser(
        prog="drive.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--world", required=True,
                     help="MATTER_WORLD value (case-insensitive world name)")
    ap.add_argument("--timeline", required=True,
                     help="path to the command-timeline file to run")
    ap.add_argument("--out-dir", required=True,
                     help="directory for cmd.txt and log.txt (and usually screenshots)")
    ap.add_argument("--timeout", type=float, default=600.0,
                     help="seconds to wait before killing the editor (default 600)")
    ap.add_argument("--editor", default=None,
                     help="path to editor.exe (default: "
                          "MatterEditor/build/windows/editor.exe next to this repo)")
    ap.add_argument("--hide-ui", action="store_true",
                     help="set MATTER_HIDE_UI=1 for the editor process")
    ap.add_argument("--env", action="append", default=[], metavar="K=V",
                     help="extra environment variable (repeatable); applied "
                          "after -- and can override -- the defaults above")
    return ap.parse_args(argv)


def repo_paths():
    # This file lives at MatterEngine3/tools/drive.py -> repo root is two
    # levels up, and MatterEditor is a sibling of MatterEngine3.
    here = Path(__file__).resolve()
    repo_root = here.parents[2]
    editor_dir = repo_root / "MatterEditor"
    return repo_root, editor_dir


def kill_process_tree(proc):
    if proc.poll() is not None:
        return
    if os.name == "nt":
        # editor.exe does not normally spawn children, but taskkill /T is
        # the cheap, dependency-free way to be sure on Windows.
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        proc.kill()


def pump_output(proc, log_file):
    """Tee the child's stdout to our own stdout and to log_file, line by
    line, until the child closes its stdout (i.e. exits)."""
    try:
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            log_file.write(line)
            log_file.flush()
    except (ValueError, OSError):
        pass  # pipe torn down under us by a timeout kill; nothing left to read


def extract_shot_lines(timeline_path):
    """Returns [(verb, path_text)] for every shot/shot_now line, file order."""
    shots = []
    with open(timeline_path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            stripped = raw.strip()
            if not stripped:
                continue
            parts = stripped.split(None, 1)
            verb = parts[0]
            if verb in ("shot", "shot_now") and len(parts) == 2:
                shots.append((verb, parts[1].strip()))
    return shots


def resolve_shot_path(path_text, editor_dir):
    p = Path(path_text)
    return p if p.is_absolute() else (editor_dir / p).resolve()


def has_quit_line(timeline_path):
    """True if the timeline contains a bare `quit` line (D-17)."""
    with open(timeline_path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            if raw.strip() == "quit":
                return True
    return False


def unlink_stale_shot_artifacts(shots, editor_dir):
    """D-09: remove any PNG/.done pair from a PREVIOUS run before this one
    launches, so a run that fails to actually produce a shot cannot
    false-pass on leftovers. Returns the count actually removed."""
    removed = 0
    for _verb, path_text in shots:
        png_path = resolve_shot_path(path_text, editor_dir)
        done_path = png_path.with_name(png_path.name + ".done")
        for p in (png_path, done_path):
            try:
                p.unlink()
                removed += 1
            except FileNotFoundError:
                pass
    return removed


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])
    _, editor_dir = repo_paths()

    editor_exe = (Path(args.editor).resolve() if args.editor
                  else (editor_dir / "build" / "windows" / "editor.exe").resolve())
    if not editor_exe.exists():
        print(f"drive.py: editor not found at {editor_exe}", file=sys.stderr)
        return 2

    timeline_path = Path(args.timeline).resolve()
    if not timeline_path.exists():
        print(f"drive.py: timeline not found at {timeline_path}", file=sys.stderr)
        return 2

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd_txt = (out_dir / "cmd.txt").resolve()
    shutil.copyfile(timeline_path, cmd_txt)

    # D-09: computed once, up front, and reused for both the pre-launch
    # unlink below and the post-run existence check -- the same list, so
    # "removed before" and "checked after" can never drift apart.
    shots = extract_shot_lines(timeline_path)
    removed = unlink_stale_shot_artifacts(shots, editor_dir)
    if removed:
        print(f"drive.py: removed {removed} stale artifact(s) from a previous run "
              "into this --out-dir")

    # D-17: non-fatal -- --timeout is always the backstop.
    if not has_quit_line(timeline_path):
        print("drive.py: WARNING -- timeline has no `quit` line; the editor "
              f"only stops via --timeout ({args.timeout}s) or being closed "
              "some other way", file=sys.stderr)

    tmp_dir = tempfile.gettempdir()
    env = os.environ.copy()
    env["MATTER_WORLD"] = args.world
    env["MATTER_CMD_FIFO"] = str(cmd_txt)
    env["TMP"] = tmp_dir
    env["TEMP"] = tmp_dir
    if args.hide_ui:
        env["MATTER_HIDE_UI"] = "1"
    for kv in args.env:
        if "=" not in kv:
            print(f"drive.py: --env expects K=V, got '{kv}'", file=sys.stderr)
            return 2
        k, v = kv.split("=", 1)
        env[k] = v

    log_path = out_dir / "log.txt"
    print(f"drive.py: editor={editor_exe}")
    print(f"drive.py: cwd={editor_dir} world={args.world} cmd_fifo={cmd_txt}")
    print(f"drive.py: log -> {log_path}")

    timed_out = False
    with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
        proc = subprocess.Popen(
            [str(editor_exe)],
            cwd=str(editor_dir),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        pump_thread = threading.Thread(target=pump_output, args=(proc, log_file), daemon=True)
        pump_thread.start()

        deadline = time.monotonic() + args.timeout
        while proc.poll() is None:
            if time.monotonic() >= deadline:
                timed_out = True
                break
            time.sleep(0.2)

        if timed_out:
            print(f"drive.py: TIMEOUT after {args.timeout}s -- killing process tree",
                  file=sys.stderr)
            kill_process_tree(proc)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass

        pump_thread.join(timeout=5)

    if timed_out:
        return 2

    exit_code = proc.returncode
    print(f"drive.py: editor exited with code {exit_code}")

    # Reuse the SAME `shots` list the pre-launch unlink used (D-09) -- not
    # recomputed -- so what got cleared and what gets checked can never see
    # a different timeline.
    missing = []
    for _verb, path_text in shots:
        png_path = resolve_shot_path(path_text, editor_dir)
        done_path = png_path.with_name(png_path.name + ".done")
        if not png_path.exists():
            missing.append(str(png_path))
        if not done_path.exists():
            missing.append(str(done_path))

    if missing:
        print(f"drive.py: FAIL -- {len(missing)} expected file(s) missing:")
        for m in missing:
            print(f"  {m}")
        return 1

    if exit_code != 0:
        print(f"drive.py: FAIL -- editor exited with code {exit_code} "
              "(screenshots were all present, but a crash/FATAL mid-script "
              "must not read as a pass)")
        return 1

    print(f"drive.py: OK -- editor exit=0, {len(shots)} shot(s) verified "
          f"({len(shots) * 2} files: PNG + .done)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
