#!/usr/bin/env python3
"""Native/browser equivalence and result-lifetime harness.

Materialises every case in cases.json from the frozen component-separation
fixtures and runs it on three geometry backends and three authoring hosts:

  native    matter_parity_native: the production C ABI (Release build)
  node      the production WASM adapter under Node (node_wasm.mjs)
  chromium  the same adapter, the portable authoring SDK and the worker
            runtime in headless Chromium (browser/page.html)

Authoring must be byte-identical across the native QuickJS host, Node and
Chromium. Native and WASM step outcomes must agree exactly except geometry
numbers governed by the case's comparison-policy profile; Node and Chromium
must agree line for line. Every outcome is also judged against the fixture's
expected code or analytic oracle, and the whole observation set is compared
with the pinned baseline.json. The harness never writes inside the corpus and
has no update mode: it writes candidate-baseline.json to --out, and adopting
it is a reviewed change to baseline.json plus BASELINE_SHA256 below.

A case whose only mismatch is a declared oracle_divergence (the exact first
mismatch a frozen oracle produces against production, with its reason) is
DIVERGENT: reported distinctly, not a failure. Any other mismatch, or a
declared divergence that stops reproducing, is a FAIL.

Exit codes: 0 PASS (unsupported profiles and declared divergences are listed
distinctly), 1 FAIL, 2 configuration error, 3 a required tool is unavailable.
"""
import argparse
from functools import partial
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import threading

PARITY = Path(__file__).resolve().parent
sys.path.insert(0, str(PARITY.parent))
from verify import CORPUS, REPO, compare_geometry, near, require  # noqa: E402

# sha256 of baseline.json. Changing the baseline without changing this pin is
# a FAIL, so a regression cannot be accepted by overwriting expected output.
BASELINE_SHA256 = "fda18136f359bf150c48a52deadb2ffeaab503a403f1b8291d8ecbee58060ac2"
BASELINE = PARITY / "baseline.json"
BASELINE_SCHEMA = "matter-parity-baseline-v1"

STATUS = {"ok": 0, "invalid_input": 1, "unsupported_capability": 2, "unsupported_version": 3,
          "missing_dependency": 4, "cycle": 5, "resource_limit": 6, "cancelled": 7, "stale_result": 8,
          "backend_failure": 9, "invalid_handle": 10}
BACKENDS = ("native", "wasm")
RUNNER_KEYS = {"op", "instance", "as", "request", "job", "budget", "cancel", "emit", "source", "result",
               "limits", "expected", "count", "steps", "snapshot"}
HARNESS_KEYS = {"expect", "expect_native", "expect_wasm", "checks", "oracle", "mesh_oracles", "label", "note",
                "only", "budget_case", "cancel_case", "late_case", "late_job", "expected_late", "diagnostics",
                "oracle_divergence"}
CANCEL_MODES = {"before_checkpoint": "pre", "progress_callback": "progress"}
CALL_OPS = {"evaluate", "query", "process"}
CHECK_KEYS = {"same_wire_as": {"result"}, "empty_result": set(), "material_triangles": {"expected"},
              "materials": {"expected"}, "affine": {"instance", "transform", "flattened"},
              "shared_instances": {"prototype", "ids", "spacing_x"}, "seeded_volume": {"sample", "material"},
              "query_samples": {"expected"}, "same_field_as": {"step", "field"}, "published_bytes_of": {"result"}}
PROFILE_CHECKS = {"affine", "shared_instances", "seeded_volume", "query_samples"}
# Wire keys whose numbers a profile governs; the class is inherited by every
# number beneath the key. Everything else (IDs, indices, counts, materials,
# transforms, jobs) is compared exactly.
TOLERANCE_KEYS = {"positions": "position_metres", "bounds": "position_metres", "position": "position_metres",
                  "distance_metres": "position_metres", "normals": "normal_components",
                  "tangents": "normal_components", "normal": "normal_components", "uv_sets": "uv"}
IGNORED_BETWEEN_BACKENDS = {"progress_events", "handle", "distinct_handles"}
# The binding's default execution budget is 5 s of wall-clock time, so on a
# loaded host an equivalence step could turn into resource_limit. Every geometry
# call (and the browser runtime's jobs) carries this explicit ceiling instead;
# no budget fixture targets execution time, and budget fixtures still set theirs.
EXECUTION_CEILING = {"execution_microseconds": 120_000_000}
LOCATION = re.compile(r" at \S+:\d+:\d+$")
MISSING = object()


class ConfigError(Exception):
    """The corpus or cases.json is inconsistent; no verdict is possible."""


class Unavailable(Exception):
    """A required tool is missing; an unavailable check is not a pass."""


class Blocked(Exception):
    """A case cannot be materialised because an upstream outcome failed."""


def compact(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def sha_text(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def sha_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def hex16(value):
    if type(value) is int and 0 <= value < 1 << 64:
        return f"{value:016x}"
    if isinstance(value, str) and re.fullmatch(r"[0-9a-f]{16}", value):
        return value
    raise ConfigError(f"generation_epoch {value!r} is not a 64-bit integer")


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def strict_equal(a, b):
    """Deep equality that never conflates booleans with numbers."""
    if isinstance(a, bool) or isinstance(b, bool):
        return type(a) is type(b) and a == b
    if is_number(a) and is_number(b):
        return a == b
    if isinstance(a, dict) and isinstance(b, dict):
        return list(a) == list(b) and all(strict_equal(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(strict_equal(x, y) for x, y in zip(a, b))
    return type(a) is type(b) and a == b


def raw_wire(line):
    """Runners print the wire last and verbatim, so its exact bytes are recoverable."""
    marker = line.find(',"wire":')
    return None if marker < 0 else line[marker + len(',"wire":'):-1]


def compare_values(actual, expected, profile, path, cls, deviations, problems, limit=20):
    """Walks two parsed wires; keys, lengths, types and ungoverned values are exact."""
    if len(problems) >= limit:
        return
    if isinstance(expected, dict):
        if not isinstance(actual, dict) or list(actual) != list(expected):
            problems.append(f"{path}: keys {list(actual) if isinstance(actual, dict) else type(actual).__name__}"
                            f" != {list(expected)}")
            return
        for key in expected:
            compare_values(actual[key], expected[key], profile, f"{path}.{key}", TOLERANCE_KEYS.get(key, cls),
                           deviations, problems, limit)
    elif isinstance(expected, list):
        if not isinstance(actual, list) or len(actual) != len(expected):
            problems.append(f"{path}: length {len(actual) if isinstance(actual, list) else type(actual).__name__}"
                            f" != {len(expected)}")
            return
        for i, (a, e) in enumerate(zip(actual, expected)):
            compare_values(a, e, profile, f"{path}[{i}]", cls, deviations, problems, limit)
    elif is_number(expected) and is_number(actual) and cls and profile and cls in profile:
        try:
            near(actual, expected, profile[cls], path)
        except ValueError as error:
            problems.append(f"{error} ({cls} {profile[cls]})")
            return
        deviations[cls] = max(deviations.get(cls, 0.0), abs(actual - expected))
    elif not strict_equal(actual, expected):
        problems.append(f"{path}: {actual!r} != {expected!r}")


def governed_zeroed(value, cls=None):
    if isinstance(value, dict):
        return {k: governed_zeroed(v, TOLERANCE_KEYS.get(k, cls)) for k, v in value.items()}
    if isinstance(value, list):
        return [governed_zeroed(v, cls) for v in value]
    if cls and is_number(value):
        return 0
    return value


def metadata_digest(wire):
    """Digest of every exact part of a wire: profile-governed numbers are zeroed."""
    return sha_text(compact(governed_zeroed(wire)))


class Corpus:
    """Every input file the harness reads, with its digest for the baseline."""

    def __init__(self):
        self.inputs = {}
        self.cases = self.json(PARITY / "cases.json")
        if self.cases.get("schema") != "matter-parity-cases-v1":
            raise ConfigError("unsupported cases.json schema")
        jobs = self.json(CORPUS / self.cases["envelope"])
        self.envelope = {k: jobs["envelope"][k] for k in ("request_id", "source_revision")}
        self.envelope["generation_epoch"] = hex16(jobs["envelope"]["generation_epoch"])
        self.budgets = {b["id"]: b for b in jobs["budgets"]}
        self.cancellations = {c["id"]: c for c in jobs["cancellations"]}
        self.late = {c["id"]: c for c in jobs["late_results"]}
        self.policy = self.json(CORPUS / self.cases["comparison_policy"])
        self.oracles = self.json(CORPUS / "fixtures/geometry-oracles.json")
        self.validation = {c["id"]: c for c in self.json(CORPUS / "fixtures/validation-cases.json")["cases"]}
        self.seeded = {s["seed"]: s for s in self.json(CORPUS / "fixtures/seeded-variation.json")["samples"]}
        self.rng = {v["seed"]: v for v in self.json(CORPUS / "fixtures/rng-splitmix64.json")["vectors"]}
        self.manifest = self.json(CORPUS / "manifest.json")
        self.generators = self.text(PARITY / "authoring/generators.mjs")

    def text(self, path):
        path = Path(path)
        try:
            data = path.read_bytes()
        except OSError as error:
            raise ConfigError(f"cannot read {path}: {error}") from error
        self.inputs[path.relative_to(REPO).as_posix()] = hashlib.sha256(data).hexdigest()
        return data.decode("utf-8")

    def json(self, path):
        try:
            return json.loads(self.text(path))
        except json.JSONDecodeError as error:
            raise ConfigError(f"{path}: {error}") from error

    def profile(self, name):
        if name is None:
            return None
        if name not in self.policy["profiles"]:
            raise ConfigError(f"unknown comparison profile {name}")
        return self.policy["profiles"][name]

    def late_envelope(self, late_id):
        case = self.late.get(late_id) or _config(f"unknown late case {late_id}")
        job = dict(self.envelope)
        if case["field"] not in job:
            raise ConfigError(f"{late_id}: unknown job field {case['field']}")
        job[case["field"]] = hex16(case["replacement"]) if case["field"] == "generation_epoch" else case["replacement"]
        return job


def _config(message):
    raise ConfigError(message)


class Step:
    def __init__(self, index, runner, only, expect, checks, oracle, mesh_oracles, diagnostics, label, divergence):
        self.index, self.runner, self.only, self.expect = index, runner, only, expect
        self.checks, self.oracle, self.mesh_oracles = checks, oracle, mesh_oracles
        self.diagnostics, self.label, self.divergence = diagnostics, label, divergence

    def describe(self):
        name = self.runner.get("as") or self.runner.get("result") or self.runner.get("instance") or ""
        return f"#{self.index} {self.runner['op']} {name}" + (f" ({self.label})" if self.label else "")


class Run:
    def __init__(self, run_id, kind, entry, profile_name):
        self.id, self.kind, self.entry, self.profile_name = run_id, kind, entry, profile_name
        self.steps, self.blocked, self.backends = [], None, BACKENDS

    def steps_for(self, backend):
        return [s for s in self.steps if backend in s.only]


class Materialiser:
    """Turns cases.json steps into runner steps; fixtures supply every expected code."""

    def __init__(self, corpus, authored):
        self.corpus, self.authored = corpus, authored

    def status(self, value, op):
        if op == "admit":
            if value in (0, "ok"):
                return 0
            if value in STATUS:
                return value
        elif value in STATUS:
            return STATUS[value]
        elif type(value) is int and value in STATUS.values():
            return value
        raise ConfigError(f"unknown status {value!r} for {op}")

    def request(self, spec):
        """Returns (request text, expected code supplied by the fixture or None)."""
        if not isinstance(spec, dict) or not spec:
            raise ConfigError(f"bad request spec {spec!r}")
        keys = set(spec)

        def only(*allowed):
            if not keys <= set(allowed):
                raise ConfigError(f"request spec keys {sorted(keys - set(allowed))} unsupported with {allowed[0]}")
        if "recipe_file" in spec:
            only("recipe_file", "add_required")
            recipe = self.corpus.json(CORPUS / spec["recipe_file"])
            if "add_required" in spec:
                if not isinstance(recipe.get("required_capabilities"), list):
                    raise ConfigError(f"{spec['recipe_file']} has no required_capabilities list")
                recipe["required_capabilities"].extend(spec["add_required"])
            return compact({"binding_version": 1, "recipe": recipe}), None
        if "authored" in spec:
            only("authored")
            return '{"binding_version":1,"recipe":' + self.authored(spec["authored"]) + "}", None
        if "validation" in spec:
            only("validation")
            case = self.corpus.validation.get(spec["validation"]) or _config(f"unknown validation case {spec['validation']}")
            if case["kind"] == "recipe":
                text = compact({"binding_version": 1, "recipe": self.corpus.json(CORPUS / case["input"])})
            elif case["kind"] == "json":
                text = self.corpus.text(CORPUS / case["input"]).strip()
            else:
                raise ConfigError(f"{case['id']}: {case['kind']} inputs cannot be sent through the recipe binding")
            return text, case["expected_code"]
        if "embed" in spec:
            edits = keys & {"replace", "replace_last", "truncate"}
            only("embed", "replace", "replace_last", "truncate")
            if len(edits) != 1:
                raise ConfigError("embed needs exactly one of replace/replace_last/truncate")
            text, code = self.request(spec["embed"])
            if code is not None:
                raise ConfigError("embedded validation requests would carry two expected codes")
            if "truncate" in spec:
                return text[:spec["truncate"]], None
            return substitute(text, spec.get("replace") or spec["replace_last"], "replace_last" in spec), None
        if "text" in spec:
            only("text")
            return spec["text"], None
        if "query" in spec:
            only("query", "replace")
            query = self.corpus.cases["queries"].get(spec["query"]) or _config(f"unknown query {spec['query']}")
            text = compact(query)
            return (substitute(text, spec["replace"], False) if "replace" in spec else text), None
        if "asset_file" in spec:
            only("asset_file")
            return compact({"binding_version": 1, "asset": self.corpus.json(CORPUS / spec["asset_file"])}), None
        raise ConfigError(f"unknown request spec {sorted(keys)}")

    def run_steps(self, run, raw_steps, profile):
        published = None
        for index, raw in enumerate(raw_steps):
            unknown = set(raw) - RUNNER_KEYS - HARNESS_KEYS
            if unknown:
                raise ConfigError(f"{run.id} step {index}: unknown keys {sorted(unknown)}")
            op = raw.get("op") or _config(f"{run.id} step {index}: missing op")
            only = raw.get("only", list(BACKENDS))
            if not only or not set(only) <= set(BACKENDS):
                raise ConfigError(f"{run.id} step {index}: bad only {only}")
            runner = {k: v for k, v in raw.items() if k in RUNNER_KEYS}
            supplied = None

            def supply(code, why):
                nonlocal supplied
                if supplied is not None and supplied != code:
                    raise ConfigError(f"{run.id} step {index}: {why} conflicts with {supplied}")
                supplied = code
            if op in CALL_OPS:
                text, code = self.request(raw.get("request") or _config(f"{run.id} step {index}: missing request"))
                runner["request"] = text
                if code is not None:
                    supply(code, "validation fixture")
                runner["job"] = self.corpus.late_envelope(raw["late_job"]) if "late_job" in raw else dict(self.corpus.envelope)
                budget = dict(raw.get("budget", {}))
                if "budget_case" in raw:
                    case = self.corpus.budgets.get(raw["budget_case"]) or _config(f"unknown budget case {raw['budget_case']}")
                    if case["field"] in budget:
                        raise ConfigError(f"{run.id} step {index}: budget field {case['field']} given twice")
                    budget[case["field"]] = case["ceiling"]
                    supply(case["expected_code"], "budget fixture")
                runner["budget"] = {**EXECUTION_CEILING, **budget}
                if "cancel_case" in raw:
                    case = self.corpus.cancellations.get(raw["cancel_case"]) or _config(f"unknown cancel case {raw['cancel_case']}")
                    runner["cancel"] = CANCEL_MODES.get(case["when"]) or _config(f"unknown cancellation point {case['when']}")
                    if runner["cancel"] == "progress" and only != ["native"]:
                        raise ConfigError(f"{run.id} step {index}: progress cancellation exists only on the native ABI")
                    supply(case["expected_code"], "cancellation fixture")
            elif any(k in raw for k in ("request", "budget_case", "cancel_case", "late_job")):
                raise ConfigError(f"{run.id} step {index}: request fields on {op}")
            extra = {}
            if op == "admit":
                if "late_case" in raw:
                    case = self.corpus.late.get(raw["late_case"]) or _config(f"unknown late case {raw['late_case']}")
                    runner["expected"] = self.corpus.late_envelope(raw["late_case"])
                    if published is None:
                        raise ConfigError(f"{run.id} step {index}: {raw['late_case']} needs an earlier admitted result")
                    supply(case["expected_code"], "late fixture")
                    extra = {"codes": [case["expected_code"]],
                             "published": published if case.get("preserve_previous") else _config("unpreserved late case")}
                elif "expected_late" in raw:
                    runner["expected"] = self.corpus.late_envelope(raw["expected_late"])
                elif raw.get("expected") == {}:
                    runner["expected"] = dict(self.corpus.envelope)
                else:
                    raise ConfigError(f"{run.id} step {index}: admit needs expected {{}}, late_case or expected_late")
            if op == "repeat":
                runner["steps"] = [self.inner(run, index, inner) for inner in raw.get("steps", [])]
                if not runner["steps"]:
                    raise ConfigError(f"{run.id} step {index}: empty repeat")
            base = dict(raw.get("expect", {}))
            if supplied is not None:
                if "status" in base and self.status(base["status"], op) != self.status(supplied, op):
                    raise ConfigError(f"{run.id} step {index}: expect.status contradicts the fixture's {supplied}")
                base["status"] = supplied
            base = {**extra, **base}
            expect = {}
            for backend in only:
                merged = {**base, **raw.get(f"expect_{backend}", {})}
                if "status" not in merged:
                    if op not in ("create", "heap"):
                        raise ConfigError(f"{run.id} step {index}: no expected status for {op}")
                    merged["status"] = 0
                merged["status"] = self.status(merged["status"], op)
                expect[backend] = merged
            checks = raw.get("checks", [])
            for check in checks:
                kind = check.get("kind")
                if kind not in CHECK_KEYS:
                    raise ConfigError(f"{run.id} step {index}: unknown check {kind}")
                if not CHECK_KEYS[kind] <= set(check):
                    raise ConfigError(f"{run.id} step {index}: {kind} needs {sorted(CHECK_KEYS[kind])}")
                if kind in PROFILE_CHECKS and profile is None:
                    raise ConfigError(f"{run.id}: {kind} needs a comparison profile")
                if kind == "seeded_volume" and check["sample"] not in self.corpus.seeded:
                    raise ConfigError(f"{run.id}: unknown seeded sample {check['sample']}")
            if ("oracle" in raw or "mesh_oracles" in raw) and profile is None:
                raise ConfigError(f"{run.id}: geometry oracles need a comparison profile")
            if "oracle" in raw and raw["oracle"] not in self.corpus.oracles:
                raise ConfigError(f"{run.id}: unknown oracle {raw['oracle']}")
            if raw.get("diagnostics") not in (None, "adapter"):
                raise ConfigError(f"{run.id} step {index}: diagnostics must be 'adapter'")
            # A declared divergence names one oracle on this step and the exact
            # first mismatch it produces; any other mismatch still fails.
            names = ([raw["oracle"]] if "oracle" in raw else []) + [o.get("mesh") for o in raw.get("mesh_oracles", [])]
            for name, entry in raw.get("oracle_divergence", {}).items():
                if name not in names:
                    raise ConfigError(f"{run.id} step {index}: divergence for {name}, which is not an oracle here")
                if set(entry) != {"problem", "reason"} or not all(isinstance(v, str) and v for v in entry.values()):
                    raise ConfigError(f"{run.id} step {index}: divergence {name} needs a problem and a reason")
            if op == "admit" and expect.get("native", {}).get("status") == 0:
                published = raw["result"]
            run.steps.append(Step(index, runner, only, expect, checks, raw.get("oracle"), raw.get("mesh_oracles", []),
                                  raw.get("diagnostics"), raw.get("label"), raw.get("oracle_divergence", {})))

    def inner(self, run, index, raw):
        if set(raw) - RUNNER_KEYS or raw.get("op") == "repeat":
            raise ConfigError(f"{run.id} step {index}: repeat steps take runner keys only and cannot nest")
        inner = dict(raw)
        if raw.get("op") in CALL_OPS:
            inner["request"], code = self.request(raw.get("request") or _config("repeat step missing request"))
            if code is not None:
                raise ConfigError(f"{run.id} step {index}: repeat steps carry no expectations")
            inner["job"] = dict(self.corpus.envelope)
            inner["budget"] = {**EXECUTION_CEILING, **raw.get("budget", {})}
        return inner


def substitute(text, pair, last):
    old, new = pair
    if old not in text:
        raise ConfigError(f"replacement target {old!r} not in request")
    if last:
        at = text.rindex(old)
        return text[:at] + new + text[at + len(old):]
    return text.replace(old, new, 1)


def build_plan(corpus, authored):
    """Every step run. `authored` maps an A-* id to native canonical bytes."""
    cases = corpus.cases
    mat = Materialiser(corpus, authored)
    runs = []
    preflight = Run("PREFLIGHT", "preflight", {"id": "PREFLIGHT", "fixture_ids": []}, None)
    mat.run_steps(preflight, [{"op": "create", "instance": "a"},
                              {"op": "capabilities", "instance": "a", "expect": {"status": "ok"}}], None)
    runs.append(preflight)
    for entry in cases["geometry"]:
        run = Run(entry["id"], "geometry", entry, entry.get("profile"))
        try:
            mat.run_steps(run, entry["steps"], corpus.profile(entry.get("profile")))
        except Blocked as error:
            run.blocked, run.steps = str(error), []
        runs.append(run)
    for entry in cases["unsupported"]:
        run = Run(entry["id"], "unsupported", entry, None)
        mat.run_steps(run, [{"op": "create", "instance": "u"},
                            {"op": "capabilities", "instance": "u", "expect": {"status": "ok"}},
                            {"op": "evaluate", "instance": "u", "as": "r", "request": entry["request"],
                             "expect": entry["expect"] if "message" not in entry["expect"]
                             else {k: v for k, v in entry["expect"].items() if k != "message"}}], None)
        runs.append(run)
    # Native wires for the worker-runtime corpus, under the exact envelope the
    # browser runtime uses, so R-CORPUS compares byte for byte.
    reference = cases_entry(cases["runtime"], "R-CORPUS")
    ref = Run("RUNTIME-REFERENCE", "reference", {"id": "RUNTIME-REFERENCE", "fixture_ids": []}, reference.get("profile"))
    ref.backends = ("native",)
    steps = [{"op": "create", "instance": "a", "only": ["native"]}]
    for name in reference["generators"]:
        author_id = authoring_id(cases, name, reference["seed"])
        steps.append({"op": "evaluate", "instance": "a", "as": name, "only": ["native"],
                      "request": {"authored": author_id}, "expect": {"status": "ok"}})
    try:
        mat.run_steps(ref, steps, corpus.profile(reference.get("profile")))
    except Blocked as error:
        ref.blocked, ref.steps = str(error), []
    runs.append(ref)
    return runs


def cases_entry(entries, entry_id):
    for entry in entries:
        if entry["id"] == entry_id:
            return entry
    raise ConfigError(f"missing case {entry_id}")


def authoring_id(cases, generator, seed):
    for entry in cases["authoring"]:
        if entry["generator"] == generator and entry["seed"] == seed and not entry.get("parameters"):
            return entry["id"]
    raise ConfigError(f"no authoring case for {generator}@{seed}")


# ---------------------------------------------------------------- execution

def run_process(cmd, stdin_text, cwd=None, timeout=900):
    try:
        return subprocess.run(cmd, input=stdin_text, capture_output=True, text=True, encoding="utf-8",
                              cwd=cwd, timeout=timeout)
    except FileNotFoundError as error:
        raise Unavailable(f"cannot start {cmd[0]}: {error}") from error
    except subprocess.TimeoutExpired as error:
        return subprocess.CompletedProcess(cmd, -1, error.stdout or "", f"timed out after {timeout} s")


def native_author(host, corpus, request):
    entry = ("import * as sdk from 'matter:authoring';\nimport {%s} from './generators.mjs';\n"
             "export default ctx => %s(sdk, ctx);\n") % (request["generator"], request["generator"])
    document = {"entry": "fixture/main.mjs", "seed": request["seed"], "parameters": request["parameters"], "modules": [
        {"id": "fixture/main.mjs", "uri": "fixture://main", "dependencies": ["fixture/generators.mjs"], "source": entry},
        {"id": "fixture/generators.mjs", "uri": "fixture://generators", "dependencies": [], "source": corpus.generators}]}
    done = run_process([str(host), "--stdin"], json.dumps(document), timeout=120)
    if done.returncode == 0:
        out = done.stdout[:-1] if done.stdout.endswith("\n") else done.stdout
        return {"status": "ok", "canonical": out}
    lines = [line for line in done.stderr.splitlines() if line.strip()]
    # host_tests.cpp prints "FAIL <diagnostics>"; with one diagnostic the
    # prefix lands on the line that carries its code.
    last = lines[-1].removeprefix("FAIL ") if lines else ""
    match = re.fullmatch(r"([a-z_]+): (.*)", last) if done.returncode == 1 and lines else None
    if match:
        return {"status": "error", "code": match.group(1), "message": LOCATION.sub("", match.group(2))}
    return {"status": "host_failure", "exit": done.returncode, "stderr": done.stderr[-2000:]}


def run_steps_native(runner, steps):
    done = run_process([str(runner)], "".join(compact(s) + "\n" for s in steps))
    lines = done.stdout.splitlines()
    if done.returncode != 0 or len(lines) != len(steps):
        return None, f"native runner exit {done.returncode}, {len(lines)}/{len(steps)} lines: {done.stderr.strip()[-600:]}"
    return lines, None


def run_steps_node(node, module, steps):
    done = run_process([node, str(PARITY / "node_wasm.mjs"), str(module)],
                       "".join(compact(s) + "\n" for s in steps), cwd=REPO)
    lines = done.stdout.splitlines()
    if done.returncode != 0 or len(lines) != len(steps):
        return None, f"node exit {done.returncode}, {len(lines)}/{len(steps)} lines: {done.stderr.strip()[-600:]}"
    return lines, None


def run_chromium(args, plan):
    from playwright.sync_api import sync_playwright
    module, dist = args.wasm_module.resolve(), args.runtime_dir.resolve()
    plan_bytes = compact(plan).encode("utf-8")

    class Handler(SimpleHTTPRequestHandler):
        def translate_path(self, path):
            clean = path.split("?", 1)[0].split("#", 1)[0]
            if clean == "/wasm/matter_geometry_wasm.mjs":
                return str(module)
            if clean == "/wasm/matter_geometry_wasm.wasm":
                return str(module.with_suffix(".wasm"))
            if clean.startswith("/integrations/web-procedural/runtime/dist/"):
                return str(dist / Path(clean).name)
            return super().translate_path(path)

        def do_GET(self):
            if self.path.split("?", 1)[0] == "/__parity/plan.json":
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(plan_bytes)))
                self.end_headers()
                self.wfile.write(plan_bytes)
                return
            super().do_GET()

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler, directory=str(REPO)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    errors, environment = [], {}
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(executable_path=str(args.browser), headless=True,
                                                 args=["--no-sandbox", "--disable-background-networking"])
            environment["browser"] = browser.version
            page = browser.new_page()
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.goto(f"http://127.0.0.1:{server.server_port}/tests/component-separation/parity/browser/page.html")
            page.wait_for_function("window.parityReport || window.parityFailure", timeout=900000)
            failure = page.evaluate("window.parityFailure")
            report = None if failure else page.evaluate("window.parityReport")
            browser.close()
    finally:
        server.shutdown()
    if failure or errors:
        return None, f"chromium page failed: {failure or ''} {errors}".strip(), environment
    return report, None, environment


# ------------------------------------------------------------------ judging

class Verdicts:
    def __init__(self):
        self.cases = {}

    def case(self, case_id, kind, entry, profile_name=None, profile=None):
        self.cases[case_id] = {"kind": kind, "verdict": None, "problems": [], "fixture_ids": entry.get("fixture_ids", []),
                               "mandatory_case": entry.get("mandatory_case"), "profile": profile_name,
                               "tolerances": profile, "max_deviation": {}, "geometry_reports": [], "divergences": [],
                               "steps": []}
        return self.cases[case_id]


def mesh_by_id(wire, mesh_id):
    for mesh in wire["asset"]["meshes"]:
        if mesh["id"] == mesh_id:
            return mesh
    raise ValueError(f"mesh {mesh_id} missing")


def flatten(mesh, m):
    require(m[12:] == [0, 0, 0, 1], "affine transform bottom row")
    positions = [[m[0]*x + m[1]*y + m[2]*z + m[3], m[4]*x + m[5]*y + m[6]*z + m[7], m[8]*x + m[9]*y + m[10]*z + m[11]]
                 for x, y, z in mesh["positions"]]
    det = m[0]*(m[5]*m[10] - m[6]*m[9]) - m[1]*(m[4]*m[10] - m[6]*m[8]) + m[2]*(m[4]*m[9] - m[5]*m[8])
    indices = list(mesh["indices"])
    if det < 0:
        for i in range(0, len(indices), 3):
            indices[i+1], indices[i+2] = indices[i+2], indices[i+1]
    bounds = {"min": [min(p[a] for p in positions) for a in range(3)],
              "max": [max(p[a] for p in positions) for a in range(3)]}
    return {**mesh, "positions": positions, "indices": indices, "bounds": bounds}


def run_check(check, parsed, raw, seen, corpus, profile):
    """Raises ValueError on a failed check; returns geometry evidence, if any."""
    kind = check["kind"]
    wire = json.loads(raw) if raw is not None else None
    needs_wire = kind not in ("same_field_as", "published_bytes_of", "same_wire_as")
    require(wire is not None or not needs_wire, "no wire emitted")
    if kind == "same_wire_as":
        other = seen.get(check["result"])
        require(other is not None and other[1] is not None, f"no earlier wire named {check['result']}")
        require(raw == other[1], f"wire differs from {check['result']}")
    elif kind == "empty_result":
        total = sum(len(m["indices"]) for m in wire["asset"]["meshes"])
        require(total == 0, f"{total} indices in an expected-empty result")
    elif kind == "material_triangles":
        counts = {}
        for mesh in wire["asset"]["meshes"]:
            for sub in mesh["submeshes"]:
                counts[sub["material"]] = counts.get(sub["material"], 0) + sub["index_count"] // 3
        require(counts == check["expected"], f"material triangles {counts} != {check['expected']}")
    elif kind == "materials":
        found = {m["id"]: m for m in wire["asset"]["materials"]}
        require(sorted(found) == sorted(check["expected"]), f"material ids {sorted(found)}")
        for mid, want in check["expected"].items():
            got = found[mid]
            pairs = [("baseColor", got["base_color"]["factor"], [f32(x) for x in want["baseColor"]])]
            pairs += [(k, got[k], f32(want[k])) for k in ("metallic", "roughness") if k in want]
            for name, actual, expected in pairs:
                require(strict_equal(actual, expected), f"{mid}.{name} {actual} != {expected}")
    elif kind == "affine":
        instance = next((i for i in wire["asset"]["instances"] if i["id"] == check["instance"]), None)
        require(instance is not None, f"instance {check['instance']} missing")
        require(strict_equal(instance["local_transform"], check["transform"]), f"local_transform {instance['local_transform']}")
        flat = flatten(mesh_by_id(wire, instance["prototype"]), instance["local_transform"])
        return {"check": "affine", **compare_geometry({"meshes": [flat]}, check["flattened"], profile)}
    elif kind == "shared_instances":
        require([m["id"] for m in wire["asset"]["meshes"]] == [check["prototype"]], "shared prototype must be stored once")
        instances = wire["asset"]["instances"]
        require([i["id"] for i in instances] == check["ids"], f"instance ids {[i['id'] for i in instances]}")
        for n, instance in enumerate(instances):
            require(instance["prototype"] == check["prototype"], f"{instance['id']} prototype {instance['prototype']}")
            want = [1, 0, 0, check["spacing_x"] * n, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
            require(strict_equal(instance["local_transform"], want), f"{instance['id']} transform {instance['local_transform']}")
    elif kind == "seeded_volume":
        sample = corpus.seeded[check["sample"]]
        h = sample["height_metres"]
        oracle = {"bounds": {"min": [-0.5, -h / 2, -0.5], "max": [0.5, h / 2, 0.5]},
                  "volume_metres3": sample["expected_volume_metres3"], "material": check["material"]}
        return {"check": "seeded_volume", "seed": check["sample"], **compare_geometry(wire["asset"], oracle, profile)}
    elif kind == "query_samples":
        samples = wire["samples"]
        require(len(samples) == len(check["expected"]), f"{len(samples)} samples")
        for n, (got, want) in enumerate(zip(samples, check["expected"])):
            require(got["hit"] is want["hit"], f"sample {n} hit {got['hit']}")
            for key, rule in (("position", "position_metres"), ("normal", "normal_components")):
                require(len(got[key]) == 3, f"sample {n} {key}")
                for a, e in zip(got[key], want[key]):
                    near(a, e, profile[rule], f"sample {n} {key}")
            near(got["distance_metres"], want["distance_metres"], profile["position_metres"], f"sample {n} distance")
    elif kind == "same_field_as":
        other = seen.get(check["step"])
        require(other is not None, f"no earlier step named {check['step']}")
        require(strict_equal(parsed.get(check["field"]), other[0].get(check["field"])),
                f"{check['field']} {parsed.get(check['field'])} != {other[0].get(check['field'])}")
    elif kind == "published_bytes_of":
        other = seen.get(check["result"])
        require(other is not None and "wire_bytes" in other[0], f"no earlier wire named {check['result']}")
        require(parsed.get("published_bytes") == other[0]["wire_bytes"],
                f"published {parsed.get('published_bytes')} bytes, {check['result']} has {other[0]['wire_bytes']}")
    return None


def judge_backend(run, backend, lines, corpus, record):
    profile = corpus.profile(run.profile_name)
    seen = {}
    for step, line in zip(run.steps_for(backend), lines):
        where = f"{backend} {step.describe()}"
        try:
            parsed = json.loads(line)
        except json.JSONDecodeError:
            record["problems"].append(f"{where}: unparseable outcome {line[:200]}")
            continue
        ok = True
        for key, want in step.expect[backend].items():
            got = parsed.get(key, MISSING)
            if got is MISSING or not strict_equal(got, want):
                ok = False
                detail = f"; diagnostics {compact(parsed.get('diagnostics', parsed.get('message')))[:300]}" \
                    if "diagnostics" in parsed or "message" in parsed else ""
                record["problems"].append(f"{where}: {key} {'missing' if got is MISSING else repr(got)} != "
                                          f"expected {want!r}{detail}")
        raw = raw_wire(line)
        if ok:
            for check in step.checks:
                try:
                    evidence = run_check(check, parsed, raw, seen, corpus, profile)
                    if evidence:
                        record["geometry_reports"].append({"backend": backend, "step": step.describe(), **evidence})
                except (ValueError, KeyError, TypeError, IndexError) as error:
                    record["problems"].append(f"{where}: {check['kind']}: {error}")
            oracles = ([(step.oracle, None, corpus.oracles[step.oracle])] if step.oracle else []) + \
                [(o["mesh"], o["mesh"], o) for o in step.mesh_oracles]
            for name, mesh_id, oracle in oracles:
                declared = step.divergence.get(name)
                try:
                    wire = json.loads(raw)
                    asset = {"meshes": [mesh_by_id(wire, mesh_id)]} if mesh_id else wire["asset"]
                    evidence = compare_geometry(asset, oracle, profile)
                    record["geometry_reports"].append({"backend": backend, "step": step.describe(), "oracle": name, **evidence})
                    if declared:
                        record["problems"].append(f"{where}: oracle {name}: declared divergence "
                                                  f"{declared['problem']!r} no longer reproduces; remove it")
                except (ValueError, KeyError, TypeError, IndexError) as error:
                    if declared and str(error) == declared["problem"]:
                        record["divergences"].append({"backend": backend, "step": step.describe(), "oracle": name,
                                                      "problem": str(error), "reason": declared["reason"]})
                    else:
                        record["problems"].append(f"{where}: oracle {name}: {error}")
        if "as" in step.runner and parsed.get("status") == 0:
            seen[step.runner["as"]] = (parsed, raw)


def compare_backends(run, native_lines, wasm_lines, corpus, record):
    profile = corpus.profile(run.profile_name)
    native = {s.index: l for s, l in zip(run.steps_for("native"), native_lines)}
    wasm = {s.index: l for s, l in zip(run.steps_for("wasm"), wasm_lines)}
    deviations = record["max_deviation"].setdefault("native_vs_wasm", {})
    wires = record.setdefault("wires_compared", {"identical_bytes": 0, "within_tolerance": 0})
    for step in run.steps:
        if step.index not in native or step.index not in wasm:
            continue
        where = f"native/wasm {step.describe()}"
        try:
            n, w = json.loads(native[step.index]), json.loads(wasm[step.index])
        except json.JSONDecodeError:
            continue
        nraw, wraw = raw_wire(native[step.index]), raw_wire(wasm[step.index])
        ignored = set(IGNORED_BETWEEN_BACKENDS)
        # Fields the case expects to differ were already judged per backend.
        ignored |= {k for k in set(step.expect["native"]) | set(step.expect["wasm"])
                    if step.expect["native"].get(k, MISSING) != step.expect["wasm"].get(k, MISSING)}
        if step.diagnostics == "adapter":
            ignored.add("diagnostics")
        if nraw != wraw:
            ignored.add("wire_bytes")
        nkeys, wkeys = [k for k in n if k not in ignored], [k for k in w if k not in ignored]
        if sorted(nkeys) != sorted(wkeys):
            record["problems"].append(f"{where}: fields {nkeys} != {wkeys}")
            continue
        for key in nkeys:
            if key == "wire" and nraw != wraw:
                problems = []
                compare_values(w["wire"], n["wire"], profile, "wire", None, deviations, problems)
                record["problems"].extend(f"{where}: {p}" for p in problems)
                wires["within_tolerance"] += not problems
            elif key == "wire":
                wires["identical_bytes"] += 1
            elif not strict_equal(n[key], w[key]):
                record["problems"].append(f"{where}: {key} native {compact(n[key])[:300]} != wasm {compact(w[key])[:300]}")


def judge_runs(runs, outputs, corpus, verdicts, capabilities):
    for run in runs:
        record = verdicts.case(run.id, run.kind, run.entry, run.profile_name, corpus.profile(run.profile_name))
        if run.blocked:
            record["problems"].append(f"blocked: {run.blocked}")
            continue
        got = outputs[run.id]
        for family in ("native", "node", "chromium"):
            if family == "native" or "wasm" in run.backends:
                if got.get(f"{family}_error"):
                    record["problems"].append(got[f"{family}_error"])
        lines = {"native": got.get("native"), "wasm": got.get("node")}
        if "wasm" in run.backends and got.get("node") is not None and got.get("chromium") is not None:
            node, chrome = got["node"], got["chromium"]
            if len(node) != len(chrome):
                record["problems"].append(f"node/chromium: {len(node)} != {len(chrome)} lines")
            for step, a, b in zip(run.steps_for("wasm"), node, chrome):
                if a != b:
                    record["problems"].append(f"node/chromium {step.describe()}: {a[:200]} != {b[:200]}")
        for backend in run.backends:
            if lines[backend] is not None:
                judge_backend(run, backend, lines[backend], corpus, record)
        if lines["native"] is not None and lines["wasm"] is not None and "wasm" in run.backends:
            compare_backends(run, lines["native"], lines["wasm"], corpus, record)
        for step in run.steps:
            record["steps"].append({"step": step.describe(), "only": step.only,
                                    "expect": {b: step.expect[b].get("status") for b in step.only}})
        # Unknown required capabilities fail explicitly, never by omission.
        if capabilities is not None:
            for step in run.steps:
                if step.runner["op"] == "evaluate" and all(step.expect[b]["status"] == 0 for b in step.only):
                    recipe = json.loads(step.runner["request"]).get("recipe", {})
                    for cap in recipe.get("required_capabilities", []):
                        if (cap["name"], cap["version"]) not in capabilities:
                            record["problems"].append(f"{step.describe()}: requires {cap['name']}@{cap['version']}, "
                                                      "which no backend advertises")


def judge_unsupported(runs, outputs, verdicts):
    for run in runs:
        if run.kind != "unsupported":
            continue
        record, entry = verdicts.cases[run.id], run.entry
        record["reason"] = entry.get("reason")
        evaluated = {}
        for backend, family in (("native", "native"), ("wasm", "node")):
            lines = outputs[run.id].get(family)
            if lines is None:
                continue
            caps = json.loads(raw_wire(lines[1]) or "[]") if lines[1].startswith('{"status":0') else []
            evaluate = json.loads(lines[2])
            evaluated[backend] = evaluate.get("status")
            prefix = entry.get("absent_prefix")
            if prefix and any(c["name"].startswith(prefix) for c in caps):
                record["problems"].append(f"{backend}: advertises a {prefix}* capability; implement the comparison")
            message = entry["expect"].get("message")
            if message and message not in compact(evaluate.get("diagnostics")):
                record["problems"].append(f"{backend}: diagnostics lack {message!r}: {compact(evaluate.get('diagnostics'))[:300]}")
        if 0 in evaluated.values():
            record["problems"].insert(0, "now supported; implement the comparison instead of the unsupported probe")
        record["observed"] = evaluated


def judge_authoring(corpus, outcomes, verdicts):
    for entry in corpus.cases["authoring"]:
        record = verdicts.case(entry["id"], "authoring", entry)
        got = {host: outcomes[host].get(entry["id"]) for host in ("native", "node", "chromium")}
        record["outcome"] = got["native"]
        for host in ("node", "chromium"):
            if got[host] is None:
                record["problems"].append(f"{host}: no outcome")
            elif got[host] != got["native"]:
                record["problems"].append(f"{host} outcome differs from native: {compact(got[host])[:300]} != "
                                          f"{compact(got['native'])[:300]}")
        native = got["native"]
        if native is None:
            record["problems"].append("native: no outcome")
            continue
        if "expect_error" in entry:
            if native.get("status") != "error" or native.get("code") != entry["expect_error"]:
                record["problems"].append(f"expected authoring error {entry['expect_error']}, got {compact(native)[:300]}")
            continue
        if native.get("status") != "ok":
            record["problems"].append(f"authoring failed: {compact(native)[:400]}")
            continue
        canonical = native["canonical"]
        record["canonical_sha256"] = sha_text(canonical)
        if "canonical_file" in entry and canonical != corpus.text(CORPUS / entry["canonical_file"]):
            record["problems"].append(f"canonical bytes differ from {entry['canonical_file']}")
        if "rng_vector" in entry or "seeded_sample" in entry:
            metadata = json.loads(canonical).get("metadata", {})
            if "rng_vector" in entry:
                vector = corpus.rng.get(entry["rng_vector"]) or _config(f"unknown rng vector {entry['rng_vector']}")
                n = len(metadata.get("words", []))
                if n == 0 or metadata["words"] != vector["words"][:n] or not strict_equal(metadata["units"], vector["units"][:n]):
                    record["problems"].append(f"RNG words/units differ from rng-splitmix64 vector {entry['rng_vector']}")
            if "seeded_sample" in entry:
                sample = corpus.seeded.get(entry["seeded_sample"]) or _config(f"unknown seeded sample {entry['seeded_sample']}")
                if not metadata.get("units") or metadata["units"][0] != sample["first_unit"]:
                    record["problems"].append("first unit draw differs from seeded-variation.json")


def judge_runtime(corpus, runtime, reference, verdicts):
    entries = {e["id"]: e for e in corpus.cases["runtime"]}
    for entry in corpus.cases["runtime"]:
        verdicts.case(entry["id"], "runtime", entry, entries["R-CORPUS"].get("profile"),
                      corpus.profile(entries["R-CORPUS"].get("profile")))
    if runtime is None:
        for entry in corpus.cases["runtime"]:
            verdicts.cases[entry["id"]]["problems"].append("chromium produced no runtime observations")
        return
    profile = corpus.profile(entries["R-CORPUS"].get("profile"))

    def check(case_id, condition, message):
        if not condition:
            verdicts.cases[case_id]["problems"].append(message)

    def sans_job(text):
        value = json.loads(text)
        value.pop("job", None)
        return value
    corpus_obs = runtime.get("R-CORPUS", {})
    deviations = verdicts.cases["R-CORPUS"]["max_deviation"].setdefault("runtime_vs_native", {})
    for name in entries["R-CORPUS"]["generators"]:
        got = corpus_obs.get(name)
        if got is None:
            check("R-CORPUS", False, f"{name}: no observation")
            continue
        check("R-CORPUS", got.get("stages") == 1, f"{name}: {got.get('stages')} stages")
        want = reference.get(name)
        if want is None:
            check("R-CORPUS", False, f"{name}: no native reference wire")
        elif got["wire"] != want:
            problems = []
            compare_values(json.loads(got["wire"]), json.loads(want), profile, f"{name}.wire", None, deviations, problems)
            for problem in problems:
                check("R-CORPUS", False, problem)
    primitive = corpus_obs.get("primitive", {}).get("wire")
    if primitive is None:
        primitive_value = None
    else:
        primitive_value = sans_job(primitive)

    def same_primitive(case_id, text, label):
        check(case_id, primitive_value is not None and text is not None and strict_equal(sans_job(text), primitive_value),
              f"{label} differs from the R-CORPUS primitive wire")
    staged = runtime.get("R-STAGED", {})
    check("R-STAGED", staged.get("stages") == 3, f"{staged.get('stages')} stages, expected evaluate+query+evaluate")
    placed = {i["id"]: i["local_transform"] for i in staged.get("instances", [])}
    check("R-STAGED", strict_equal(placed.get("on.surface"), [1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1]),
          f"detail placed at {placed.get('on.surface')}, expected the ray hit (0, 1, 0)")
    # The staged source's detail box names no material, so it carries the
    # recipe builder's default; asserting that keeps a silent default change visible.
    try:
        wire = json.loads(staged["wire"])
        evidence = compare_geometry({"meshes": [mesh_by_id(wire, "detail")]},
                                    {"bounds": {"min": [-0.125] * 3, "max": [0.125] * 3}, "volume_metres3": 0.015625,
                                     "material": "geometry.default"}, profile)
        verdicts.cases["R-STAGED"]["geometry_reports"].append({"backend": "chromium-runtime", "oracle": "detail", **evidence})
    except (ValueError, KeyError, TypeError, IndexError) as error:
        check("R-STAGED", False, f"detail oracle: {error}")
    for case_id, code in (("R-CANCEL-BEFORE", "cancelled"), ("R-CANCEL-RUNNING", "cancelled"),
                          ("R-DEADLINE", "resource_limit"), ("R-RESTART", "cancelled")):
        got = runtime.get(case_id, {})
        check(case_id, got.get("code") == code, f"code {got.get('code')} != {code}")
        same_primitive(case_id, got.get("after", {}).get("wire"), "the following job's wire")
    check("R-RESTART", (runtime.get("R-RESTART", {}).get("restarts") or 0) >= 1, "restart() replaced no worker")
    stale = runtime.get("R-STALE", {})
    late = {"source_revision": corpus.late_envelope("LATE-REVISION")["source_revision"],
            "generation_epoch": corpus.late_envelope("LATE-EPOCH")["generation_epoch"]}
    check("R-STALE", stale.get("running") == "stale_result", f"running job {stale.get('running')}")
    check("R-STALE", stale.get("submitted") == "stale_result", f"old-envelope submission {stale.get('submitted')}")
    job = stale.get("current_job") or {}
    check("R-STALE", job.get("source_revision") == late["source_revision"] and job.get("generation_epoch") == late["generation_epoch"],
          f"current job {job} lacks the new envelope {late}")
    same_primitive("R-STALE", stale.get("current", {}).get("wire"), "the current-envelope wire")
    own = runtime.get("R-OWNERSHIP", {})
    check("R-OWNERSHIP", own.get("iterations") == 50, f"{own.get('iterations')} iterations")
    for key in ("heaps_equal", "positions_identical", "transfer_detached_source", "transfer_clone_intact",
                "kept_survives_dispose"):
        check("R-OWNERSHIP", own.get(key) is True, f"{key} is {own.get(key)}")
    same_primitive("R-OWNERSHIP", own.get("last_wire"), "the 50th job's wire")
    errors = runtime.get("R-AUTHOR-ERRORS", {})
    check("R-AUTHOR-ERRORS", errors.get("unknownCapability") == "unsupported_capability",
          f"unknown capability {errors.get('unknownCapability')}")
    check("R-AUTHOR-ERRORS", errors.get("repeatedAssembly") == "missing_dependency",
          f"pinned assembly child {errors.get('repeatedAssembly')}")
    disposed = runtime.get("R-DISPOSE", {})
    check("R-DISPOSE", disposed.get("running") == "disposed" and disposed.get("submitted") == "disposed",
          f"after dispose: running {disposed.get('running')}, submitted {disposed.get('submitted')}")
    stats = disposed.get("stats") or {}
    check("R-DISPOSE", stats.get("active") == 0 and stats.get("queued") == 0, f"stats after dispose {stats}")


def coverage(corpus, verdicts):
    manifest = corpus.manifest
    sample_ids = {s["id"] for s in manifest["samples"]}
    used = set()
    for section in ("authoring", "geometry", "runtime", "unsupported"):
        for entry in corpus.cases[section]:
            unknown = set(entry.get("fixture_ids", [])) - sample_ids
            if unknown:
                raise ConfigError(f"{entry['id']}: fixture ids {sorted(unknown)} are not manifest samples")
            used |= set(entry.get("fixture_ids", []))
    mandatory = {}
    for case_id, record in verdicts.cases.items():
        if record["mandatory_case"]:
            if record["mandatory_case"] in mandatory:
                raise ConfigError(f"mandatory case {record['mandatory_case']} mapped twice")
            mandatory[record["mandatory_case"]] = case_id
    missing = [c for c in manifest["mandatory_cases"] if c not in mandatory]
    if missing:
        raise ConfigError(f"mandatory cases without a parity case: {missing}")
    table = [{"mandatory_case": c, "case": mandatory[c], "verdict": verdicts.cases[mandatory[c]]["verdict"]}
             for c in manifest["mandatory_cases"]]
    return table, sorted(sample_ids - used)


# ----------------------------------------------------------------- baseline

def wire_ref(text, wires):
    # The exact wire bytes are kept as one string per wire: reviewable as one
    # line each, and re-hashable, so the stored evidence proves its own key.
    digest = sha_text(text)
    wires.setdefault(digest, text)
    return {"sha256": digest, "metadata": metadata_digest(json.loads(text))}


def normalise_line(line, wires):
    parsed = json.loads(line)
    parsed.pop("handle", None)
    parsed.pop("heap_bytes", None)
    raw = raw_wire(line)
    if raw is not None:
        parsed["wire"] = wire_ref(raw, wires)
    return parsed


def candidate_baseline(corpus, runs, outputs, outcomes, runtime, verdicts):
    wires = {}
    geometry = {}
    for run in runs:
        got = outputs.get(run.id, {})
        geometry[run.id] = {"profile": run.profile_name}
        for backend, family in (("native", "native"), ("wasm", "node")):
            if backend in run.backends and got.get(family) is not None:
                geometry[run.id][backend] = [{"step": s.describe(), **normalise_line(l, wires)}
                                             for s, l in zip(run.steps_for(backend), got[family])]
    observed = None
    if runtime is not None:
        def obs(o, *keys):
            return {k: o.get(k) for k in keys}
        observed = {"R-CORPUS": {n: {"wire": wire_ref(o["wire"], wires), "stages": o["stages"]}
                                 for n, o in runtime.get("R-CORPUS", {}).items()},
                    "R-STAGED": {"wire": wire_ref(runtime["R-STAGED"]["wire"], wires), "stages": runtime["R-STAGED"]["stages"],
                                 "instances": runtime["R-STAGED"]["instances"]}}
        for case_id in ("R-CANCEL-BEFORE", "R-CANCEL-RUNNING", "R-DEADLINE", "R-RESTART"):
            o = runtime[case_id]
            observed[case_id] = {**obs(o, "code", *(["restarts"] if "restarts" in o else [])),
                                 "after": {"wire": wire_ref(o["after"]["wire"], wires), "stages": o["after"]["stages"]}}
        stale = runtime["R-STALE"]
        observed["R-STALE"] = {**obs(stale, "running", "submitted", "current_job"),
                               "current": {"wire": wire_ref(stale["current"]["wire"], wires)}}
        own = runtime["R-OWNERSHIP"]
        observed["R-OWNERSHIP"] = {**{k: v for k, v in own.items() if k not in ("heap_bytes", "last_wire")},
                                   "last_wire": wire_ref(own["last_wire"], wires)}
        observed["R-AUTHOR-ERRORS"] = runtime["R-AUTHOR-ERRORS"]
        observed["R-DISPOSE"] = {**obs(runtime["R-DISPOSE"], "running", "submitted"),
                                 "stats": obs(runtime["R-DISPOSE"]["stats"], "active", "queued")}
    return {"schema": BASELINE_SCHEMA,
            "about": "Pinned parity observations. Adopt a new candidate only by review: copy candidate-baseline.json here "
                     "and update BASELINE_SHA256 in run_parity.py in the same change.",
            "inputs": dict(sorted(corpus.inputs.items())),
            "authoring": {k: outcomes["native"].get(k) for k in sorted(outcomes["native"])},
            "unsupported": {k: {"verdict": v["verdict"], "observed": v.get("observed")}
                            for k, v in verdicts.cases.items() if v["kind"] == "unsupported"},
            "geometry": geometry, "runtime": observed, "wires": dict(sorted(wires.items()))}


def diff_baseline(base, cand, path, profile, base_wires, cand_wires, problems, deviations):
    if len(problems) >= 40:
        return
    if isinstance(base, dict) and isinstance(cand, dict) and set(base) == {"sha256", "metadata"} == set(cand):
        if base["sha256"] == cand["sha256"]:
            return
        if base["metadata"] != cand["metadata"]:
            problems.append(f"{path}: exact wire metadata changed")
            return
        found = []
        compare_values(json.loads(cand_wires[cand["sha256"]]), json.loads(base_wires[base["sha256"]]), profile, path,
                       None, deviations, found)
        problems.extend(found)
        return
    if isinstance(base, dict) and isinstance(cand, dict):
        if sorted(base) != sorted(cand):
            problems.append(f"{path}: fields {sorted(base)} != {sorted(cand)}")
            return
        wire_moved = isinstance(base.get("wire"), dict) and isinstance(cand.get("wire"), dict) and \
            base["wire"].get("sha256") != cand["wire"].get("sha256")
        for key in base:
            if key == "wire_bytes" and wire_moved:
                continue
            diff_baseline(base[key], cand[key], f"{path}.{key}", profile, base_wires, cand_wires, problems, deviations)
    elif isinstance(base, list) and isinstance(cand, list):
        if len(base) != len(cand):
            problems.append(f"{path}: {len(base)} entries became {len(cand)}")
            return
        for i, (b, c) in enumerate(zip(base, cand)):
            diff_baseline(b, c, f"{path}[{i}]", profile, base_wires, cand_wires, problems, deviations)
    elif not strict_equal(base, cand):
        problems.append(f"{path}: {compact(base)[:200]} became {compact(cand)[:200]}")


def judge_baseline(corpus, candidate, verdicts):
    status = {"path": BASELINE.relative_to(REPO).as_posix(), "pinned_sha256": BASELINE_SHA256, "problems": [],
              "max_deviation": {}}
    if not BASELINE.is_file():
        status["problems"].append("no baseline.json: review candidate-baseline.json, copy it in and pin its sha256")
        return status
    data = BASELINE.read_bytes()
    status["sha256"] = hashlib.sha256(data).hexdigest()
    if status["sha256"] != BASELINE_SHA256:
        status["problems"].append("baseline.json does not match BASELINE_SHA256; expected output may not change unpinned")
        return status
    base = json.loads(data)
    if base.get("schema") != BASELINE_SCHEMA:
        status["problems"].append(f"baseline schema {base.get('schema')}")
        return status
    changed = sorted(k for k in set(base["inputs"]) | set(candidate["inputs"])
                     if base["inputs"].get(k) != candidate["inputs"].get(k))
    if changed:
        status["problems"].append(f"inputs changed since the baseline was pinned: {changed}; review the candidate and re-pin")
        return status
    runtime_profile = corpus.profile(cases_entry(corpus.cases["runtime"], "R-CORPUS").get("profile"))
    problems = status["problems"]
    corrupt = sorted(k for k, text in base["wires"].items() if not isinstance(text, str) or sha_text(text) != k)
    if corrupt:
        problems.append(f"baseline wires whose bytes no longer hash to their key: {corrupt}")
        return status
    for section in ("authoring", "unsupported"):
        diff_baseline(base[section], candidate[section], section, None, {}, {}, problems, {})
    if sorted(base["geometry"]) != sorted(candidate["geometry"]):
        problems.append(f"geometry cases {sorted(base['geometry'])} != {sorted(candidate['geometry'])}")
    for case_id in sorted(set(base["geometry"]) & set(candidate["geometry"])):
        deviations = status["max_deviation"].setdefault(case_id, {})
        profile = corpus.profile(candidate["geometry"][case_id]["profile"])
        diff_baseline(base["geometry"][case_id], candidate["geometry"][case_id], f"geometry.{case_id}", profile,
                      base["wires"], candidate["wires"], problems, deviations)
    if candidate["runtime"] is None:
        problems.append("runtime: no observations to compare")
    else:
        diff_baseline(base["runtime"], candidate["runtime"], "runtime", runtime_profile, base["wires"],
                      candidate["wires"], problems, status["max_deviation"].setdefault("runtime", {}))
    return status


# --------------------------------------------------------------------- main

def tool_fingerprints(args):
    files = {"native_runner": args.native_runner, "native_host": args.native_host, "wasm_module": args.wasm_module,
             "wasm_binary": args.wasm_module.with_suffix(".wasm"), "runtime_index": args.runtime_dir / "index.mjs",
             "runtime_worker": args.runtime_dir / "worker.mjs"}
    return {k: {"path": str(Path(v).resolve()), "sha256": sha_file(v)} for k, v in files.items() if Path(v).is_file()}


def execute(args):
    out = args.out.resolve()
    if out == CORPUS or CORPUS in out.parents:
        raise ConfigError("--out must be outside tests/component-separation; the harness never writes into the corpus")
    missing = [f"{name} {path}" for name, path in (("--native-runner", args.native_runner), ("--native-host", args.native_host),
                                                   ("--wasm-module", args.wasm_module),
                                                   ("WASM binary", args.wasm_module.with_suffix(".wasm")),
                                                   ("--runtime-dir index.mjs", args.runtime_dir / "index.mjs"),
                                                   ("--browser", args.browser)) if not Path(path).is_file()]
    node = shutil.which(args.node)
    if node is None:
        missing.append(f"--node {args.node}")
    try:
        import playwright.sync_api  # noqa: F401
    except ImportError:
        missing.append(f"playwright for {sys.executable}")
    if missing:
        raise Unavailable("missing: " + "; ".join(missing))
    corpus = Corpus()
    build_plan(corpus, lambda _id: "{}")  # validate every case before running anything
    out.mkdir(parents=True, exist_ok=True)
    raw_dir = out / "raw"
    raw_dir.mkdir(exist_ok=True)

    requests = [{"id": e["id"], "generator": e["generator"], "seed": e["seed"], "parameters": e.get("parameters", {})}
                for e in corpus.cases["authoring"]]
    outcomes = {"native": {r["id"]: native_author(args.native_host, corpus, r) for r in requests}}
    done = run_process([node, str(PARITY / "authoring/node_author.mjs")],
                       json.dumps([{k: r[k] for k in ("generator", "seed", "parameters")} for r in requests]), cwd=REPO)
    try:
        outcomes["node"] = dict(zip([r["id"] for r in requests], json.loads(done.stdout)))
    except json.JSONDecodeError:
        outcomes["node"] = {}
        print(f"node authoring failed: {done.stderr.strip()[-600:]}", file=sys.stderr)

    def authored(author_id):
        outcome = outcomes["native"].get(author_id) or _config(f"unknown authoring case {author_id}")
        if outcome.get("status") != "ok":
            raise Blocked(f"{author_id} did not author: {compact(outcome)[:300]}")
        return outcome["canonical"]
    runs = build_plan(corpus, authored)

    outputs = {}
    for run in runs:
        got = outputs[run.id] = {}
        if run.blocked:
            continue
        native_steps = [s.runner for s in run.steps_for("native")]
        (raw_dir / f"{run.id}.native.in.jsonl").write_text("".join(compact(s) + "\n" for s in native_steps), encoding="utf-8")
        got["native"], got["native_error"] = run_steps_native(args.native_runner, native_steps)
        if got["native"] is not None:
            (raw_dir / f"{run.id}.native.out.jsonl").write_text("\n".join(got["native"]) + "\n", encoding="utf-8")
        if "wasm" in run.backends:
            wasm_steps = [s.runner for s in run.steps_for("wasm")]
            (raw_dir / f"{run.id}.wasm.in.jsonl").write_text("".join(compact(s) + "\n" for s in wasm_steps), encoding="utf-8")
            got["node"], got["node_error"] = run_steps_node(node, args.wasm_module, wasm_steps)
            if got["node"] is not None:
                (raw_dir / f"{run.id}.node.out.jsonl").write_text("\n".join(got["node"]) + "\n", encoding="utf-8")

    runtime_entry = cases_entry(corpus.cases["runtime"], "R-CORPUS")
    plan = {"runs": [{"id": r.id, "steps": [s.runner for s in r.steps_for("wasm")]}
                     for r in runs if "wasm" in r.backends and not r.blocked],
            "authoring": requests,
            "runtime": {"envelope": corpus.envelope,
                        "late": {"source_revision": corpus.late_envelope("LATE-REVISION")["source_revision"],
                                 "generation_epoch": corpus.late_envelope("LATE-EPOCH")["generation_epoch"]},
                        "seed": runtime_entry["seed"], "corpus": runtime_entry["generators"],
                        "query": corpus.cases["queries"]["primitive_ray_and_closest"], "budget": EXECUTION_CEILING}}
    (out / "plan.json").write_text(json.dumps(plan, indent=1), encoding="utf-8")
    report, chromium_error, environment = run_chromium(args, plan)
    (raw_dir / "chromium.json").write_text(json.dumps(report if report is not None else {"error": chromium_error},
                                                      indent=1, ensure_ascii=False), encoding="utf-8")
    outcomes["chromium"] = (report or {}).get("authoring", {})
    for run in runs:
        if "wasm" in run.backends and not run.blocked:
            if report is None:
                outputs[run.id]["chromium_error"] = chromium_error
            else:
                outputs[run.id]["chromium"] = report["runs"].get(run.id)
                if outputs[run.id]["chromium"] is None:
                    outputs[run.id]["chromium_error"] = "chromium returned no lines"

    verdicts = Verdicts()
    capabilities, preflight = None, outputs.get("PREFLIGHT", {})
    wires = [preflight.get(f) and raw_wire(preflight[f][1]) for f in ("native", "node", "chromium")]
    if all(wires) and len(set(wires)) == 1:
        capabilities = {(c["name"], c["version"]) for c in json.loads(wires[0])}
    judge_runs(runs, outputs, corpus, verdicts, capabilities)
    if capabilities is None:
        verdicts.cases["PREFLIGHT"]["problems"].append("capability wires missing or different across native/node/chromium")
    else:
        verdicts.cases["PREFLIGHT"]["capabilities"] = json.loads(wires[0])
    judge_unsupported(runs, outputs, verdicts)
    judge_authoring(corpus, outcomes, verdicts)
    reference = {}
    for step, line in zip(next(r for r in runs if r.id == "RUNTIME-REFERENCE").steps_for("native"),
                          outputs["RUNTIME-REFERENCE"].get("native") or []):
        if step.runner["op"] == "evaluate" and raw_wire(line) is not None:
            reference[step.runner["as"]] = raw_wire(line)
    judge_runtime(corpus, (report or {}).get("runtime"), reference, verdicts)
    for record in verdicts.cases.values():
        if record["kind"] == "unsupported" and not record["problems"]:
            record["verdict"] = "UNSUPPORTED"
        elif record["problems"]:
            record["verdict"] = "FAIL"
        else:
            record["verdict"] = "DIVERGENT" if record["divergences"] else "PASS"
    table, out_of_scope = coverage(corpus, verdicts)

    candidate = candidate_baseline(corpus, runs, outputs, outcomes, (report or {}).get("runtime"), verdicts)
    (out / "candidate-baseline.json").write_text(json.dumps(candidate, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    baseline = judge_baseline(corpus, candidate, verdicts)
    failed = sorted(k for k, v in verdicts.cases.items() if v["verdict"] == "FAIL")
    unsupported = sorted(k for k, v in verdicts.cases.items() if v["verdict"] == "UNSUPPORTED")
    divergent = sorted(k for k, v in verdicts.cases.items() if v["verdict"] == "DIVERGENT")
    result = "FAIL" if failed or baseline["problems"] else "PASS"
    try:
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO, capture_output=True, text=True).stdout.strip()
    except OSError:
        head = None
    environment.update({"python": sys.version.split()[0], "platform": platform.platform(), "repo_head": head,
                        "node": run_process([node, "--version"], "").stdout.strip(),
                        "chromium_user_agent": (report or {}).get("environment", {}).get("userAgent"),
                        "tools": tool_fingerprints(args)})
    summary = {"schema": "matter-parity-report-v1", "result": result, "failed": failed, "unsupported": unsupported,
               "divergent": divergent,
               "mandatory_cases": table, "out_of_scope_samples": out_of_scope, "baseline": baseline,
               "environment": environment, "comparison_policy": corpus.cases["comparison_policy"],
               "cases": verdicts.cases}
    (out / "report.json").write_text(json.dumps(summary, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")

    for case_id, record in verdicts.cases.items():
        tolerance = ""
        if record["profile"]:
            devs = {k: v for d in record["max_deviation"].values() for k, v in d.items()}
            wires = record.get("wires_compared")
            compared = f"native/wasm wires: {wires['identical_bytes']} identical bytes, " \
                f"{wires['within_tolerance']} within tolerance; " if wires else ""
            tolerance = f" [{record['profile']}; {compared}max deviation " + \
                (", ".join(f"{k} {v:.3g}" for k, v in sorted(devs.items())) or "0") + "]"
        print(f"{record['verdict']:<11} {case_id}{tolerance}")
        if record["verdict"] == "UNSUPPORTED":
            print(f"            {record.get('reason')}")
        for d in record["divergences"]:
            print(f"            ~ {d['backend']} {d['step']} oracle {d['oracle']}: {d['problem']} ({d['reason']})")
        for problem in record["problems"][:12]:
            print(f"            - {problem}")
        if len(record["problems"]) > 12:
            print(f"            ... {len(record['problems']) - 12} more in report.json")
    print("mandatory cases:")
    for row in table:
        print(f"  {row['verdict']:<11} {row['mandatory_case']} ({row['case']})")
    print(f"baseline: {'OK' if not baseline['problems'] else 'FAIL'} ({baseline['path']})")
    for problem in baseline["problems"][:20]:
        print(f"  - {problem}")
    print(f"out of scope samples ({len(out_of_scope)}): {', '.join(out_of_scope)}")
    passed = len(verdicts.cases) - len(failed) - len(unsupported) - len(divergent)
    print(f"RESULT {result}: {passed} pass, {len(divergent)} divergent (declared oracle divergences), {len(failed)} fail, "
          f"{len(unsupported)} unsupported; report {out / 'report.json'}")
    return 0 if result == "PASS" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--native-runner", type=Path, required=True, help="matter_parity_native (Release)")
    parser.add_argument("--native-host", type=Path, required=True, help="matter_host_quickjs_tests (portable host)")
    parser.add_argument("--wasm-module", type=Path, required=True, help="matter_geometry_wasm.mjs beside its .wasm")
    parser.add_argument("--runtime-dir", type=Path, required=True, help="compiled integrations/web-procedural/runtime dist")
    parser.add_argument("--browser", type=Path, default=Path("/opt/google/chrome/chrome"))
    parser.add_argument("--node", default="node")
    parser.add_argument("--out", type=Path, required=True, help="report directory, outside the corpus")
    args = parser.parse_args()
    try:
        return execute(args)
    except ConfigError as error:
        print(f"CONFIGURATION ERROR: {error}", file=sys.stderr)
        return 2
    except Unavailable as error:
        print(f"UNAVAILABLE: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
