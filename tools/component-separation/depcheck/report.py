"""Check results, console output and the machine-readable JSON report."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

REPORT_SCHEMA = "matter.component-dependency-report.v1"
PASS, FAIL, UNTESTED = "PASS", "FAIL", "UNTESTED"


@dataclass
class Violation:
    code: str
    detail: str

    def as_dict(self) -> dict:
        return {"code": self.code, "detail": self.detail}


@dataclass
class Result:
    component: str
    variant: str
    check: str
    status: str = PASS
    violations: list[Violation] = field(default_factory=list)
    reason: str = ""
    notes: list[str] = field(default_factory=list)

    @property
    def key(self) -> str:
        return f"{self.component}/{self.variant}/{self.check}"

    def fail(self, code: str, detail: str) -> None:
        self.violations.append(Violation(code, detail))
        self.status = FAIL

    def untested(self, reason: str) -> "Result":
        self.status = UNTESTED
        self.reason = reason
        return self

    def as_dict(self) -> dict:
        data = {"component": self.component, "variant": self.variant, "check": self.check,
                "status": self.status, "violations": [v.as_dict() for v in self.violations]}
        if self.reason:
            data["reason"] = self.reason
        if self.notes:
            data["notes"] = list(self.notes)
        return data


def print_results(results: list[Result], verbose: bool = False) -> None:
    for result in results:
        line = f"{result.status:<8} {result.key}"
        if result.status == UNTESTED:
            line += f"  ({result.reason})"
        print(line)
        for violation in result.violations:
            print(f"    [{violation.code}] {violation.detail}")
        if verbose:
            for note in result.notes:
                print(f"    note: {note}")


def summarize(results: list[Result]) -> dict:
    counts = {PASS: 0, FAIL: 0, UNTESTED: 0}
    for result in results:
        counts[result.status] += 1
    return {"pass": counts[PASS], "fail": counts[FAIL], "untested": counts[UNTESTED],
            "violations": sum(len(r.violations) for r in results)}


def write_report(path: Path, results: list[Result], context: dict) -> None:
    payload = {
        "schema": REPORT_SCHEMA,
        **context,
        "summary": summarize(results),
        "results": [r.as_dict() for r in results],
        "violations": [{"check": r.key, **v.as_dict()} for r in results for v in r.violations],
        "untested": [{"check": r.key, "reason": r.reason} for r in results if r.status == UNTESTED],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
