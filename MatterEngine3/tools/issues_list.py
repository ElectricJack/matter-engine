#!/usr/bin/env python3
"""issues_list.py -- tabulate the editor's in-app issue reports
(issues/<guid>/issue.md + state.json + shot-N.png, see
docs/agent/issue-system.md) without opening the editor.

Usage:
    issues_list.py [--dir <issues-root>] [--json]

--dir resolution (when --dir is not given), mirroring issue_reporter.cpp's
own issues_dir() precedence as closely as a standalone script can:
    1. $MATTER_ISSUE_DIR, if set and non-empty.
    2. "../issues" relative to the CURRENT WORKING DIRECTORY, if that path
       exists and is a directory (this is the layout when run from
       MatterEditor/, which is where the editor itself resolves to it).
    3. Otherwise "./issues" relative to the current working directory --
       tried unconditionally as the last resort, even if it does not exist
       (an empty/missing root is reported as zero reports, not an error).

For each report directory found directly under the root:
    - short id: the first 8 hex characters of the `id:` frontmatter field
      in issue.md, falling back to the directory's own name when issue.md
      is missing/unparseable or has no `id:` field.
    - reported: the `reported:` frontmatter timestamp (UTC, ISO 8601).
    - world: the `world:` frontmatter field.
    - status: the `status:` frontmatter field.
    - shots: a COUNT OF shot-*.png FILES ON DISK, not the frontmatter
      `shots:` field -- the two can disagree (a shot removed from the
      dialog's list after an earlier one was already written to disk, or a
      hand-edited report) and the file count is the one an ingestion pass
      actually has to deal with.
    - note: the first non-blank line of the "## Report" section, or
      "(no note)" if that section is empty, missing, or holds only the
      writer's own placeholder text for an empty note.

A directory that cannot be parsed at all (no issue.md, or one that does not
even have a well-formed `---`-delimited frontmatter block) is still LISTED,
never skipped and never a crash -- every field this script could not
determine reads "(unparseable)"; the shot count is still a real count of
shot-*.png files, since that never depends on issue.md.

Sorted newest first by `reported`; rows with no usable timestamp (parse
failures) sort to the bottom.

Human-readable table by default; --json prints a JSON array to stdout and
nothing else (safe to pipe into `python -c "import json,sys; json.load(sys.stdin)"`).
"""
import argparse
import glob
import json
import os
import re
import sys


# The exact placeholder issue_reporter.cpp's write_issue_markdown() writes
# into the "## Report" section when state.note was empty at filing time (see
# write_issue_markdown in MatterEditor/src/issue_reporter.cpp) -- treated the
# same as an actually-empty note.
EMPTY_NOTE_PLACEHOLDER = "_No description given — see the shots._"

# ISO 8601 UTC, e.g. "2026-08-14T00:38:32Z" -- utc_stamp()'s exact format in
# issue_reporter.cpp. Used only to decide whether a `reported:` value is
# trustworthy enough to sort on; not re-parsed into a datetime (string sort
# on this exact format is already chronological).
_REPORTED_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")

UNPARSEABLE = "(unparseable)"
NO_NOTE = "(no note)"


def resolve_issues_root(explicit_dir):
    """--dir resolution precedence -- see the module docstring."""
    if explicit_dir:
        return explicit_dir
    env_dir = os.environ.get("MATTER_ISSUE_DIR")
    if env_dir:
        return env_dir
    if os.path.isdir("../issues"):
        return "../issues"
    return "./issues"


def parse_frontmatter(text):
    """Returns (dict, body) for a `---\\nkey: value\\n...\\n---\\n` block at
    the start of `text`, or (None, text) if the block is missing/malformed.
    Deliberately simple (no YAML types, no nesting) -- write_issue_markdown()
    only ever emits flat `key: value` scalars here."""
    lines = text.split("\n")
    if not lines or lines[0].strip() != "---":
        return None, text
    fields = {}
    i = 1
    while i < len(lines) and lines[i].strip() != "---":
        line = lines[i]
        if ":" in line:
            key, _, value = line.partition(":")
            fields[key.strip()] = value.strip()
        i += 1
    if i >= len(lines):
        return None, text  # no closing `---` -- treat the whole block as bad
    body = "\n".join(lines[i + 1:])
    return fields, body


def extract_report_note(body):
    """First non-blank line of the "## Report" section, or None if that
    section is absent, empty, or holds only the empty-note placeholder."""
    marker = "## Report"
    start = body.find(marker)
    if start < 0:
        return None
    start += len(marker)
    end = body.find("\n## ", start)
    section = body[start:end if end >= 0 else len(body)]
    for raw_line in section.split("\n"):
        line = raw_line.strip()
        if not line:
            continue
        if line == EMPTY_NOTE_PLACEHOLDER:
            return None
        return line
    return None


def count_shots(report_dir):
    return len(glob.glob(os.path.join(report_dir, "shot-*.png")))


def load_report(report_dir):
    """Best-effort parse of one issues/<dir>/ report. Never raises -- any
    failure downgrades individual fields to UNPARSEABLE rather than
    dropping the row or crashing the whole listing."""
    dir_name = os.path.basename(os.path.normpath(report_dir))
    row = {
        "dir": dir_name,
        "id": dir_name,
        "short_id": dir_name,
        "reported": UNPARSEABLE,
        "world": UNPARSEABLE,
        "status": UNPARSEABLE,
        "shots": 0,
        "note": UNPARSEABLE,
        "parse_error": None,
        "sort_key": "",
    }
    try:
        row["shots"] = count_shots(report_dir)
    except OSError as exc:
        row["parse_error"] = f"could not list shots: {exc}"

    issue_md = os.path.join(report_dir, "issue.md")
    try:
        with open(issue_md, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as exc:
        row["parse_error"] = f"could not read issue.md: {exc}"
        row["note"] = UNPARSEABLE
        return row

    try:
        fields, body = parse_frontmatter(text)
        if fields is None:
            row["parse_error"] = "issue.md has no well-formed frontmatter block"
            return row
        report_id = fields.get("id", "")
        if report_id:
            row["id"] = report_id
            row["short_id"] = report_id[:8]
        row["world"] = fields.get("world", UNPARSEABLE)
        row["status"] = fields.get("status", UNPARSEABLE)
        reported = fields.get("reported", "")
        row["reported"] = reported if reported else UNPARSEABLE
        if reported and _REPORTED_RE.match(reported):
            row["sort_key"] = reported
        note = extract_report_note(body)
        row["note"] = note if note else NO_NOTE
    except Exception as exc:  # noqa: BLE001 -- a listing tool must not crash
        row["parse_error"] = f"unexpected parse failure: {exc}"
    return row


def list_reports(root):
    reports = []
    try:
        entries = sorted(os.listdir(root))
    except OSError:
        return reports  # missing/unreadable root -> zero reports, not an error
    for name in entries:
        path = os.path.join(root, name)
        if not os.path.isdir(path):
            continue  # e.g. issues/README.md sits beside the report dirs
        reports.append(load_report(path))
    reports.sort(key=lambda r: r["sort_key"], reverse=True)
    return reports


def print_table(reports):
    if not reports:
        print("(no issue reports found)")
        return
    headers = ["ID", "REPORTED", "WORLD", "STATUS", "SHOTS", "NOTE"]
    rows = []
    for r in reports:
        rows.append([
            r["short_id"], r["reported"], r["world"], r["status"],
            str(r["shots"]), r["note"],
        ])
    widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(cell))
    # NOTE is left unclamped-but-last so a long note does not force the
    # earlier, more scannable columns to wrap in a narrow terminal.
    def fmt_row(cells):
        return "  ".join(
            cell.ljust(widths[i]) if i < len(cells) - 1 else cell
            for i, cell in enumerate(cells))
    print(fmt_row(headers))
    print(fmt_row(["-" * w for w in widths[:-1]] + ["-" * min(widths[-1], 40)]))
    for row in rows:
        print(fmt_row(row))


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="issues_list.py", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=None,
                     help="issues root directory (default: see resolution "
                          "order in the module docstring)")
    ap.add_argument("--json", action="store_true",
                     help="print a JSON array instead of a human table")
    args = ap.parse_args(argv if argv is not None else sys.argv[1:])

    root = resolve_issues_root(args.dir)
    reports = list_reports(root)

    if args.json:
        # Nothing but the JSON on stdout -- a caller piping this into
        # `json.load` must not have to filter out any other line.
        payload = [{
            "id": r["id"],
            "short_id": r["short_id"],
            "dir": r["dir"],
            "reported": r["reported"],
            "world": r["world"],
            "status": r["status"],
            "shots": r["shots"],
            "note": r["note"],
            "parse_error": r["parse_error"],
        } for r in reports]
        print(json.dumps(payload, indent=2))
    else:
        print(f"issues_list.py: {root}  ({len(reports)} report(s))",
              file=sys.stderr)
        print_table(reports)
    return 0


if __name__ == "__main__":
    sys.exit(main())
