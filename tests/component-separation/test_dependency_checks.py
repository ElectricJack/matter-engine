#!/usr/bin/env python3
"""Fixture tests for tools/component-separation/check_dependencies.py.

Every case in dependency-fixtures/cases.json copies the clean miniature
repository in dependency-fixtures/base to a temporary directory, applies a few
edits that introduce one deliberate violation, runs the checker against the
copy and compares the exit code, the exact set of violation codes and the
exact set of failing results with the case's expectations. A coverage test
proves every violation code the checker can emit is exercised by some case.

Cases marked ``toolchain`` configure and build tiny CMake probes; they are
skipped (with the reason) when cmake, ninja or a C++ compiler is missing.
Everything runs serially with one compile job.

    python3 tests/component-separation/test_dependency_checks.py
    python3 -m pytest tests/component-separation/test_dependency_checks.py -q

MATTER_DEPCHECK_KEEP_FIXTURES=1 keeps each case's temporary tree and report.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[1]
FIXTURES = HERE / "dependency-fixtures"
BASE = FIXTURES / "base"
CASES = FIXTURES / "cases.json"
TOOL = REPOSITORY / "tools" / "component-separation"
CHECKER = TOOL / "check_dependencies.py"
CASE_TIMEOUT = 600

# Codes only an unusual environment produces (an unreadable CMake File API
# reply, a ninja that cannot report dependencies); no fixture can provoke them.
ENVIRONMENT_ONLY = {"configure.file-api", "header.deps-unavailable"}
FAIL_CODE = re.compile(r'\.fail\(\s*"([a-z-]+\.[a-z-]+)"')
EXPECT_KEYS = {"exit", "codes", "failing", "untested", "passing", "stderr", "stdout", "details", "notes", "no_report"}
CASE_KEYS = {"name", "description", "toolchain", "args", "edits", "expect"}


def emitted_codes() -> set[str]:
    sources = sorted((TOOL / "depcheck").glob("*.py")) + [CHECKER]
    return {code for path in sources for code in FAIL_CODE.findall(path.read_text(encoding="utf-8"))}


def load_cases() -> list[dict]:
    data = json.loads(CASES.read_text(encoding="utf-8"))
    return data["cases"]


_TOOLCHAIN: list[str | None] = []


def toolchain_skip_reason() -> str | None:
    """Why the probe cases cannot run here, or None when they can."""
    if _TOOLCHAIN:
        return _TOOLCHAIN[0]
    reason = None
    cmake = os.environ.get("MATTER_DEPCHECK_CMAKE") or shutil.which("cmake")
    generator = os.environ.get("MATTER_DEPCHECK_GENERATOR")
    ninja = os.environ.get("MATTER_DEPCHECK_MAKE_PROGRAM") or shutil.which("ninja")
    if os.name == "nt":
        compilers = ["cl"]
    else:
        compilers = [os.environ["CXX"]] if os.environ.get("CXX") else ["c++", "g++", "clang++"]
    if not cmake:
        reason = "cmake is not on PATH (or set MATTER_DEPCHECK_CMAKE)"
    elif generator and generator != "Ninja":
        reason = f"the header checks need the Ninja generator, not {generator!r}"
    elif not ninja:
        reason = "ninja is not on PATH (or set MATTER_DEPCHECK_MAKE_PROGRAM)"
    elif not any(shutil.which(c) for c in compilers):
        reason = f"no C++ compiler found ({', '.join(compilers)})"
    _TOOLCHAIN.append(reason)
    return reason


# ------------------------------------------------------------------ edits

def _text(value) -> str:
    return "\n".join(value) + "\n" if isinstance(value, list) else value


def _inside(scratch: Path, repo: Path, rel: str) -> Path:
    path = (repo / rel).resolve()
    if path != scratch and scratch not in path.parents:
        raise ValueError(f"edit path {rel!r} leaves the case's temporary directory")
    return path


def _select(node, key):
    if isinstance(node, list):
        if isinstance(key, int):
            return node[key]
        for item in node:
            if isinstance(item, dict) and key in (item.get("name"), item.get("target"), item.get("directory")):
                return item
        raise KeyError(f"no list element named {key!r}")
    return node[key]


def apply_edit(scratch: Path, repo: Path, edit: dict) -> None:
    op = edit["op"]
    path = _inside(scratch, repo, edit["path"])
    if op in ("write", "append"):
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a" if op == "append" else "w", encoding="utf-8", newline="\n") as handle:
            handle.write(_text(edit["text"]))
    elif op == "replace":
        text = path.read_text(encoding="utf-8")
        old = _text(edit["old"]) if isinstance(edit["old"], list) else edit["old"]
        if text.count(old) != 1:
            raise ValueError(f"{edit['path']}: {old!r} occurs {text.count(old)} times, expected once")
        path.write_text(text.replace(old, _text(edit["new"]) if isinstance(edit["new"], list) else edit["new"]),
                        encoding="utf-8", newline="\n")
    elif op == "copy":
        source = _inside(scratch, repo, edit["from"])
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, path)
    elif op == "delete":
        if path.is_dir():
            shutil.rmtree(path)
        else:
            path.unlink()
    elif op == "json":
        data = json.loads(path.read_text(encoding="utf-8"))
        node = data
        for key in edit.get("at", []):
            node = _select(node, key)
        node.update(edit.get("set", {}))
        for key, values in edit.get("extend", {}).items():
            node.setdefault(key, []).extend(values)
        for key in edit.get("remove", []):
            del node[key]
        path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8", newline="\n")
    else:
        raise ValueError(f"unknown edit op {op!r}")


# ------------------------------------------------------------------ the tests

class DependencyCheckFixtures(unittest.TestCase):
    maxDiff = None

    def run_case(self, case: dict) -> None:
        if case.get("toolchain"):
            reason = toolchain_skip_reason()
            if reason:
                self.skipTest(reason)
        scratch = Path(tempfile.mkdtemp(prefix=f"depcheck-{case['name']}-")).resolve()
        keep = os.environ.get("MATTER_DEPCHECK_KEEP_FIXTURES") == "1"
        try:
            repo = scratch / "repo"
            shutil.copytree(BASE, repo)
            for edit in case.get("edits", []):
                apply_edit(scratch, repo, edit)
            report = scratch / "report.json"
            command = [sys.executable, str(CHECKER), "--root", str(repo), "--rules", str(repo / "component-rules.json"),
                       "--build-root", str(scratch / "build"), "--jobs", "1", "--report", str(report)] + case["args"]
            completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=CASE_TIMEOUT)
            stdout = completed.stdout.decode("utf-8", "replace")
            stderr = completed.stderr.decode("utf-8", "replace")
            self.check_expectations(case, completed.returncode, stdout, stderr, report)
        finally:
            if keep:
                print(f"\n[kept] {case['name']}: {scratch}", file=sys.stderr)
            else:
                shutil.rmtree(scratch, ignore_errors=True)

    def check_expectations(self, case: dict, code: int, stdout: str, stderr: str, report_path: Path) -> None:
        expect = case["expect"]
        context = f"\n--- stdout ---\n{stdout[-6000:]}\n--- stderr ---\n{stderr[-3000:]}"
        self.assertEqual(code, expect["exit"], f"exit code{context}")
        if "stderr" in expect:
            self.assertIn(expect["stderr"], stderr, context)
        if "stdout" in expect:
            self.assertIn(expect["stdout"], stdout, context)
        if expect.get("no_report"):
            self.assertFalse(report_path.exists(), "a usage or rules error must not write a report")
            return
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(report["schema"], "matter.component-dependency-report.v1")
        results = {f"{r['component']}/{r['variant']}/{r['check']}": r for r in report["results"]}
        by_status = {status: {k for k, r in results.items() if r["status"] == status} for status in ("PASS", "FAIL", "UNTESTED")}
        codes = {v["code"] for v in report["violations"]}
        self.assertEqual(sorted(codes), sorted(expect.get("codes", [])), f"violation codes{context}")
        self.assertEqual(sorted(by_status["FAIL"]), sorted(expect.get("failing", [])), f"failing results{context}")
        for key in expect.get("untested", []):
            self.assertIn(key, by_status["UNTESTED"], f"{key} should be UNTESTED{context}")
        for key in expect.get("passing", []):
            self.assertIn(key, by_status["PASS"], f"{key} should PASS{context}")
        details = [v["detail"] for v in report["violations"]]
        for fragment in expect.get("details", []):
            self.assertTrue(any(fragment in d for d in details), f"no violation detail contains {fragment!r}: {details}")
        for key, fragment in expect.get("notes", {}).items():
            self.assertIn(key, results, f"no result {key}")
            notes = results[key].get("notes", [])
            self.assertTrue(any(fragment in n for n in notes), f"{key} has no note containing {fragment!r}: {notes}")
        for entry in report["untested"]:
            self.assertTrue(entry["reason"], f"UNTESTED {entry['check']} must give a reason")


class CaseCatalogue(unittest.TestCase):
    def test_cases_are_well_formed(self):
        names = set()
        for case in load_cases():
            self.assertLessEqual(set(case), CASE_KEYS, case["name"])
            self.assertLessEqual(set(case["expect"]), EXPECT_KEYS, case["name"])
            self.assertNotIn(case["name"], names, "case names must be unique")
            self.assertRegex(case["name"], r"^[a-z0-9-]+$")
            names.add(case["name"])
            if case["expect"]["exit"] == 1:
                self.assertTrue(case["expect"].get("codes") or "--strict-untested" in case["args"],
                                f"{case['name']}: a failing case must name the violations it expects")

    def test_every_violation_code_is_exercised(self):
        emitted = emitted_codes()
        self.assertLessEqual(ENVIRONMENT_ONLY, emitted, "ENVIRONMENT_ONLY names a code the checker no longer emits")
        expected = {code for case in load_cases() for code in case["expect"].get("codes", [])}
        self.assertEqual(sorted(expected - emitted), [], "cases expect codes the checker never emits")
        self.assertEqual(sorted(emitted - ENVIRONMENT_ONLY - expected), [], "violation codes no fixture case exercises")

    def test_repository_rules_load(self):
        completed = subprocess.run([sys.executable, str(CHECKER), "--list"], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, timeout=120)
        self.assertEqual(completed.returncode, 0, completed.stderr.decode("utf-8", "replace"))


def _make_test(case: dict):
    def test(self):
        self.run_case(case)
    test.__doc__ = case.get("description")
    return test


for _case in load_cases():
    setattr(DependencyCheckFixtures, "test_" + _case["name"].replace("-", "_"), _make_test(_case))


if __name__ == "__main__":
    unittest.main()
