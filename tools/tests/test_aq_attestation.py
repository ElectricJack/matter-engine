"""Offline GitHub vectors for the delivery-branch attestation audit."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from urllib.parse import parse_qs, urlsplit


spec = importlib.util.spec_from_file_location(
    "aq_attestation", Path(__file__).parents[1] / "aq_attestation.py"
)
hosted = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = hosted
spec.loader.exec_module(hosted)

SHA = "a" * 40
OTHER_SHA = "b" * 40
APP = 5075923
PRODUCER = 15368
REPOSITORY = "example/matter-engine"
REPOSITORY_ID = 42
VERSION = "matter-native-v1"


def payload(step=None, target=None):
    checks = [
        {
            "name": name,
            "check_run_id": 100 + index,
            "check_suite_id": 900,
            "producer_app_id": PRODUCER,
            "head_sha": SHA,
            "conclusion": "success",
        }
        for index, name in enumerate(
            ("Native Windows build and tests", "Linux foundation build and tests")
        )
    ]
    runs = [{
        "workflow_run_id": 55, "run_attempt": 1, "check_suite_id": 900,
        "head_sha": SHA, "conclusion": "success",
    }]
    identity = {
        "canonical_repository_id": "matter-engine-cpp",
        "repository_id": REPOSITORY_ID,
        "ci_producer_app_id": PRODUCER,
        "attestation_app_id": APP,
    }
    if step is None:
        return {
            "schema": hosted.ATTESTATION_SCHEMA, **identity, "head_sha": SHA,
            "required_check_set_version": VERSION, "checks": checks,
            "workflow_runs": runs,
        }
    return {
        "schema": hosted.PROMOTION_SCHEMA,
        "repository": {**identity, "full_name": REPOSITORY},
        "step": step, "target_ref": f"refs/heads/{target}",
        "attestation_name": f"Agent Queue Promotion Attestation ({step})",
        "version": None, "request_id": "promotion-request",
        "batch_id": "promotion-batch", "source_sha": SHA, "base_sha": OTHER_SHA,
        "checks_version": VERSION, "checks": checks, "workflow_runs": runs,
    }


def env(step=None, target=None, **overrides):
    value = {
        "REPOSITORY": REPOSITORY, "REPOSITORY_ID": str(REPOSITORY_ID),
        "SHA": SHA, "APP_ID": str(APP), "CHECK_VERSION": VERSION,
    }
    if step is not None:
        value.update(
            ATTESTATION_NAME=f"Agent Queue Promotion Attestation ({step})",
            STEP=step, TARGET_REF=f"refs/heads/{target}",
        )
    value.update(overrides)
    return value


class GitHub:
    """Serve check-run listings and readbacks without network access."""

    def __init__(self, value=None):
        self.value = payload() if value is None else value
        self.records = [{
            "id": 7000, "name": hosted.ATTESTATION_CHECK_NAME,
            "app": {"id": APP}, "head_sha": SHA,
            "status": "completed", "conclusion": "success",
        }]
        self.checks = {
            item["check_run_id"]: {
                "id": item["check_run_id"], "name": item["name"],
                "app": {"id": item["producer_app_id"]},
                "check_suite": {"id": item["check_suite_id"]},
                "head_sha": item["head_sha"], "status": "completed",
                "conclusion": item["conclusion"],
            }
            for item in self.value["checks"]
        }
        self.requests = []
        self.error = None
        self.next_link = None
        self.refresh()

    def refresh(self, text=None):
        if text is None:
            text = json.dumps(self.value, sort_keys=True, separators=(",", ":"))
        prefix = (hosted.PROMOTION_EXTERNAL_ID_PREFIX
                  if self.value["schema"] == hosted.PROMOTION_SCHEMA
                  else hosted.EXTERNAL_ID_PREFIX)
        self.records[0].update(
            output={"text": text},
            external_id=prefix + hashlib.sha256(text.encode()).hexdigest(),
        )
        if self.value["schema"] == hosted.PROMOTION_SCHEMA:
            self.records[0]["name"] = self.value["attestation_name"]

    def __call__(self, url):
        self.requests.append(url)
        if self.error:
            raise self.error
        parts = urlsplit(url)
        assert parts.scheme == "https" and parts.netloc == "api.github.com", url
        if parts.path == f"/repos/{REPOSITORY}/commits/{SHA}/check-runs":
            query = parse_qs(parts.query)
            assert query["filter"] == ["all"]
            assert query["check_name"] == [self.records[0]["name"]]
            headers = {"link": f'<{self.next_link}>; rel="next"'} if self.next_link else {}
            return json.dumps({"check_runs": self.records}).encode(), headers
        run = self.checks[int(parts.path.rsplit("/", 1)[1])]
        return json.dumps(run).encode(), {}


class AttestationTests(unittest.TestCase):
    def assert_refused(self, github, configured=None):
        verdict = hosted.verify(env() if configured is None else configured, github)
        self.assertTrue(verdict.configured)
        self.assertFalse(verdict.attested, verdict)
        self.assertTrue(verdict.reason)
        return verdict.reason

    def test_accepts_integration_and_rechecks_each_required_check(self):
        github = GitHub()
        verdict = hosted.verify(env(), github)
        self.assertTrue(verdict.attested, verdict.reason)
        self.assertEqual(len(github.requests), 3)

    def test_accepts_both_promotion_steps(self):
        for step, target in (("staging", "staging"), ("release", "main")):
            with self.subTest(step=step):
                verdict = hosted.verify(env(step, target), GitHub(payload(step, target)))
                self.assertTrue(verdict.attested, verdict.reason)

    def test_promotion_cannot_satisfy_another_step_or_target(self):
        for overrides in (
            {"STEP": "staging"}, {"TARGET_REF": "refs/heads/dev"},
            {"CHECK_VERSION": "rotated"}, {"REPOSITORY_ID": "1"},
        ):
            with self.subTest(overrides=overrides):
                self.assert_refused(
                    GitHub(payload("release", "main")), env("release", "main", **overrides)
                )

    def test_wrong_repository_name_in_promotion_is_rejected(self):
        value = payload("release", "main")
        value["repository"]["full_name"] = "another/repository"
        self.assert_refused(GitHub(value), env("release", "main"))

    def test_integration_proof_cannot_satisfy_promotion(self):
        github = GitHub()
        github.records[0]["name"] = env("staging", "staging")["ATTESTATION_NAME"]
        self.assert_refused(github, env("staging", "staging"))

    def test_missing_configuration_does_not_make_api_calls(self):
        for key in ("APP_ID", "CHECK_VERSION"):
            with self.subTest(key=key):
                github = GitHub()
                verdict = hosted.verify(env(**{key: ""}), github)
                self.assertFalse(verdict.configured)
                self.assertFalse(verdict.attested)
                self.assertEqual(github.requests, [])

    def test_untrusted_app_and_wrong_head_are_rejected(self):
        for change in ({"app": {"id": 99}}, {"head_sha": OTHER_SHA}):
            with self.subTest(change=change):
                github = GitHub()
                github.records[0].update(change)
                self.assert_refused(github)

    def test_newest_failed_or_pending_record_overrides_old_success(self):
        for status, conclusion in (("completed", "failure"), ("in_progress", None)):
            with self.subTest(status=status):
                github = GitHub()
                newer = copy.deepcopy(github.records[0])
                newer.update(id=7001, status=status, conclusion=conclusion)
                github.records.append(newer)
                self.assertIn("7001", self.assert_refused(github))

    def test_payload_identity_is_bound_to_the_sha_repo_app_and_version(self):
        for field, value in (
            ("repository_id", 99), ("attestation_app_id", 99),
            ("head_sha", OTHER_SHA), ("required_check_set_version", "rotated"),
            ("ci_producer_app_id", APP),
        ):
            with self.subTest(field=field):
                value_to_test = payload()
                value_to_test[field] = value
                self.assert_refused(GitHub(value_to_test))

    def test_check_run_readback_failure_invalidates_proof(self):
        for change in (
            {"app": {"id": APP}}, {"head_sha": OTHER_SHA},
            {"name": "another check"}, {"conclusion": "failure"},
            {"status": "in_progress"}, {"check_suite": {"id": 901}},
        ):
            with self.subTest(change=change):
                github = GitHub()
                github.checks[100].update(change)
                self.assertIn("re-verify", self.assert_refused(github))

    def test_noncanonical_or_duplicate_key_payload_is_rejected(self):
        github = GitHub()
        canonical = github.records[0]["output"]["text"]
        for text in (
            json.dumps(github.value, indent=2), canonical + "\n",
            canonical[:-1] + ',"schema":"aq.integration-attestation.v1"}',
            "not json",
        ):
            with self.subTest(text=text[:20]):
                github.refresh(text)
                self.assert_refused(github)

    def test_digest_must_match_canonical_payload(self):
        github = GitHub()
        github.records[0]["external_id"] = hosted.EXTERNAL_ID_PREFIX + "0" * 64
        self.assertIn("digest", self.assert_refused(github))

    def test_check_and_workflow_suite_coverage_cannot_be_empty_or_duplicate(self):
        for field, entries in (("checks", []), ("workflow_runs", [])):
            with self.subTest(field=field):
                value = payload()
                value[field] = entries
                self.assert_refused(GitHub(value))
        for field in ("checks", "workflow_runs"):
            with self.subTest(duplicate=field):
                value = payload()
                value[field].append(copy.deepcopy(value[field][0]))
                self.assert_refused(GitHub(value))

    def test_api_failure_runs_fallback_instead_of_accepting(self):
        github = GitHub()
        github.error = OSError("unavailable")
        self.assert_refused(github)

    def test_pagination_cannot_send_token_to_another_host(self):
        github = GitHub()
        github.next_link = "https://untrusted.example/checks"
        self.assertIn("leaves the API", self.assert_refused(github))
        self.assertEqual(len(github.requests), 1)

    def test_invalid_configuration_is_rejected(self):
        for change in ({"APP_ID": "true"}, {"REPOSITORY_ID": "0"},
                       {"SHA": "short"}, {"GITHUB_API_URL": "http://api.github.com"}):
            with self.subTest(change=change):
                self.assert_refused(GitHub(), env(**change))

    def test_output_reason_cannot_inject_github_output_keys(self):
        github = GitHub()
        github.error = OSError("error\nattested=true\n\x00")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            summary = Path(directory) / "summary"
            result = hosted.main(env(GITHUB_OUTPUT=str(output), GITHUB_STEP_SUMMARY=str(summary)), github)
            self.assertEqual(result, 0)
            lines = output.read_text().splitlines()
            self.assertEqual(len(lines), 3)
            self.assertEqual(lines[:2], ["attested=false", "configured=true"])
            self.assertTrue(lines[2].startswith("reason="))


if __name__ == "__main__":
    unittest.main()
