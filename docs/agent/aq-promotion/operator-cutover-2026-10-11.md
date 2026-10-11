# Remaining operator steps after Jack's 2026-10-11 decision

This implements the [recorded decision](decision-2026-10-11.md), as updated by
owner direction `msg-43295073d3df42deaae6dfffb147ffe1` (sequence `10086`) and
supervisor relay `msg-6c53372280de447dafd12a6602025f16` (sequence `10088`). The
earlier `e092` bootstrap-merge handoff is superseded. Do not merge a documentation
update as the bootstrap action. Prepare and validate the required repository
code on the held branch, then use the current development delivery owner.
Report actual receipts on `fleet-ridge-94:train-migration`.

Do not run `aq integration cutover` or `cutover-plan`: this project already
targets `dev`, while those controls implement the `main -> dev` cutover and a
fixed `tests.yml` assumption. Neither retired playbook is imported, activated or
represented by a fabricated artifact. No daemon restart or branch retarget is
needed.

## 1. Generate and deliver the repository bootstrap

The supplied current policy is
`/home/jkern/.agent-queue/vault/projects/matter-engine-cpp/policy/git-first-policy.json`,
SHA256 `e7d01545c033b00d573b74d920ec9781d2ac7e4cce95b3f6fd24aca736362cdf`.
Its [committed mirror](git-first-policy.json) preserves the exact supplied bytes.
AQ PR1358, deployed revision `73509417ffeb31c197ac7419d3aa5491d196eec6`, accepts
route-free boundaries and omits absent routes from snapshots. Jack requires
matching AQ's live policy, with only the Matter check set and empty authorized
task lists substituted: parent admission `reviewed`, root admission `authorized`,
repair exhaustion `continue`, hosted CI with no authored `ci` block.

The worker's exact command below was refused with
`out of scope: integration_trust_manifest`, exit `4`, before any file write.
The supervisor then ran the authenticated generator successfully, exit `0`,
reported in `msg-dbe933c0d0654205929dfe4dcb486426` (sequence `10096`). The
[canonical base](evidence/git-first-generated-manifest.json) SHA256 is
`73bf03ce85880f5fb53800cf9ec64b8c494a462c26d9e2cf7b3e4cc0f6d7efe1`.
The worker did not retry or bypass the refusal. Generation is complete; the
command below records the requested owner operation, rather than a remaining
step to repeat unless authenticated inputs change.

```bash
set -euo pipefail
MATTER_GIT_FIRST_POLICY_FILE=/home/jkern/.agent-queue/vault/projects/matter-engine-cpp/policy/git-first-policy.json
printf '%s  %s\n' e7d01545c033b00d573b74d920ec9781d2ac7e4cce95b3f6fd24aca736362cdf "$MATTER_GIT_FIRST_POLICY_FILE" | sha256sum -c -
aq integration trust-manifest matter-engine-cpp \
  --policy "$MATTER_GIT_FIRST_POLICY_FILE" --repository-id matter-engine-cpp \
  --write .github/agent-queue-integration.json --json
```

For receipt transfer instead of writing the worker's file, the owner may set
`--write` to
`/home/jkern/.codex/worker-artifacts/fleet-ridge-94-epoch2/git-first-generated-manifest.json`
and return the complete command receipt. That transfer is now complete.
Preserve generated repository/App/check identities. The [explicit diff](evidence/git-first-manifest-reconciliation.json)
confirms that the current production bytes already reconcile the two
already-reviewed promotion names and complete
`promotion-source-audit` alias pair against the actual output and unchanged
flow. An unbound flow does not supply its promotion names to the generator;
return any resulting readiness/ordering conflict to the owner rather than
silently removing those task requirements. The supervisor has notified the
activation owner about this ordering; obtain its actual resolution.

The bootstrap includes the actual full Windows/Linux `tests.yml`, the
`main-attestation.yml` audit reading `vars.AQ_INTEGRATION_ATTESTATION_APP_ID`, and
its tested verifier. It uses the current AQ audit pattern, including configured
and attested outputs, while preserving the full fallback checks on verifier or
checkout failure. AQ's compared `train-candidate.yml` writes the legacy
`aq-train/candidate` commit status; it does not publish an App attestation.
The App-pinned git-first protection/proof path remains the configured authority.

Publish through `aq git push`, validate the actual source and deliver through
the configured development owner. The worker does not independently merge or
push `dev`, nor close this task as passing before its required cutover evidence.
Record the delivered OID, bootstrap bytes and its full exact-head checks from
App `15368`, matching run/artifact provenance, all native tests without skips,
and package validation. Qualified `8f25` evidence applies only to `8f25`; any
later source or delivery requires its own correctly scoped qualification.

## 2. Verify the completed owner setup and fresh state

The owner reports GitHub's default branch changed from `main` to `dev` and
`app-setup --apply` already set:

- `AQ_INTEGRATION_ATTESTATION_APP_ID=5075923`
- `AQ_INTEGRATION_REQUIRED_CHECK_VERSION=matter-engine-ci-v1`

The reported credential/repository/producer/variable checks passed; manifest
failed and audit workflow warned because those files were absent on `dev`.
Verify the actual delivered files and fresh runtime state. Do not repeat the
retarget or variable setup merely because the earlier handoff contained them.

```bash
set -euo pipefail
MATTER_PROMOTION_FLOW=/home/jkern/.agent-queue/vault/projects/matter-engine-cpp/policy/promotion/flow.json
gh api repos/ElectricJack/matter-engine/git/ref/heads/dev
aq integration status matter-engine-cpp --json
```

The unchanged flow SHA256 is
`1760070c181f4c148dbe0aae9e4053d1b2b18a43fef5ed2a41e686f5f1c76634`.
Reconcile actual live writers, owners, subjects and batches through supported
controls before changing modes. Disabled mode does not force-drain them.

## 3. Bind the supplied policy, observe and verify

Use a fresh integration generation for every mutation. A stale CAS refuses;
inspect the actual state rather than guessing another generation. The policy
changes from the historical proposal are explicitly authorized by the supplied
Jack/owner direction; do not bind the historical route-bearing policy or retain
its reviewed-root/human-hold settings as the current contract.

```bash
set -euo pipefail
MATTER_GIT_FIRST_POLICY_FILE=/home/jkern/.agent-queue/vault/projects/matter-engine-cpp/policy/git-first-policy.json
printf '%s  %s\n' e7d01545c033b00d573b74d920ec9781d2ac7e4cce95b3f6fd24aca736362cdf "$MATTER_GIT_FIRST_POLICY_FILE" | sha256sum -c -
MATTER_GIT_FIRST_POLICY_JSON="$(python3 -c 'import json,sys; print(json.dumps(json.load(open(sys.argv[1])), separators=(",", ":")))' "$MATTER_GIT_FIRST_POLICY_FILE")"
matter_generation() {
  aq integration status matter-engine-cpp --control-only --json |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["generation"])'
}
aq project set matter-engine-cpp integration-mode disabled \
  --expected-integration-generation "$(matter_generation)" \
  --reason 'Jack 2026-10-11: prepare route-free git-first policy matching AQ'
aq project set matter-engine-cpp integration-policy "$MATTER_GIT_FIRST_POLICY_JSON" \
  --expected-integration-generation "$(matter_generation)" \
  --reason 'bind supplied git-first-policy e7d01545 with Matter full CI pair'
aq project set matter-engine-cpp integration-mode observe \
  --expected-integration-generation "$(matter_generation)" \
  --reason 'observe supplied route-free policy before publication'
aq integration app-verify matter-engine-cpp --json
aq integration status matter-engine-cpp --json
```

Require actual intended-mode readiness, including generated identity, both
checks from producer `15368`, integration App `5075923`, variables and effective
protection without bypass. Include organization/classic protection in that
assessment. Repository administration uses the actual fresh App report fixes;
`app-setup --apply` sets differing Actions variables, not protection. Report any
manifest/flow bootstrap ordering conflict before enabling train publication.

## 4. Enable train and the unchanged flow

Proceed only after actual readiness passes. Flow activation owns create-only
staging creation from the current green `dev` head; do not manually push staging
or reset an existing target. Initial staging creation is not acceptance of the
first automatic promotion.

```bash
set -euo pipefail
aq project set matter-engine-cpp integration-mode train \
  --expected-integration-generation "$(matter_generation)" \
  --reason 'Jack 2026-10-11: enable route-free git-first delivery matching AQ'
aq project set matter-engine-cpp promotion-flow "$MATTER_PROMOTION_FLOW" \
  --expected-integration-generation "$(matter_generation)" \
  --reason 'activate unchanged dev-to-staging-to-main flow; release operator-approved'
aq promote validate --project matter-engine-cpp \
  --file "$MATTER_PROMOTION_FLOW" --remote --json
```

Return actual policy/mode/flow generations, readiness and staging-creation
receipts, with no intervening head/check change. Existing `main` is preserved.

## 5. Import, approve and activate only the two promotion artifacts

The corrected selective import command is supported below. The worker attempt
was refused at `policy_diff` scope; no import, approval or activation occurred.
`policy apply` does not support `--json`. Its actual project-specific reviews
must receive authorized decisions before activation. Existing items are not
overwritten, and export checksums are not compiled artifact identities.

```bash
set -euo pipefail
aq policy apply /home/jkern/.agent-queue/policy-profiles/agent-queue-2026-10-11 \
  --project matter-engine-cpp \
  --only playbook:project:promotion-continuous \
  --only playbook:project:promotion-request \
  --set default_branch=dev --set branch_4=staging --set branch_5=main --yes
```

After actual approval, copy the exact compiled artifact hashes into the required
variables below. No template hashes or fabricated artifacts are substituted.

```bash
set -euo pipefail
: "${MATTER_CONTINUOUS_APPROVED_ARTIFACT_SHA:?Set the actual approved compiled artifact hash}"
: "${MATTER_REQUEST_APPROVED_ARTIFACT_SHA:?Set the actual approved compiled artifact hash}"
aq playbook activate --playbook-id matter-engine-cpp.promotion-continuous \
  --artifact-sha256 "$MATTER_CONTINUOUS_APPROVED_ARTIFACT_SHA" --enabled --json
aq playbook activate --playbook-id matter-engine-cpp.promotion-request \
  --artifact-sha256 "$MATTER_REQUEST_APPROVED_ARTIFACT_SHA" --enabled --json
aq playbook activation-health --playbook-id matter-engine-cpp.promotion-continuous --json
aq playbook activation-health --playbook-id matter-engine-cpp.promotion-request --json
aq integration status matter-engine-cpp --json
aq promote status --project matter-engine-cpp --step staging --json
```

Require enabled/ready health with the approved project-scoped hashes. Return
actual train delivery and first automatic staging-promotion evidence: request or
batch, source/candidate/target OIDs, full exact-head checks from App `15368`,
staging proof from App `5075923`, and Git containment. Preserve the ordinary
refactor canary acceptance in the migration runbook under the supplied policy's
admission settings. Release remains gated by operator approval; this handoff
does not approve or initiate a release.
