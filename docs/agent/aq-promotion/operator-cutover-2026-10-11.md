# Remaining operator steps after Jack's 2026-10-11 decision

This implements the [recorded decision](decision-2026-10-11.md). Run one stage
at a time and return actual receipts to the held worker on
`fleet-ridge-94:train-migration`. These commands have not been executed by the
worker. The retired parent/root playbook imports and activations are omitted.
Review the current policy contract before any mode change; the historical
route-bearing proposal is not an approved current binding input.

## 1. Qualify and deliver the actual task head

`8f25e4b8297d8db04e992aa04f557ff10ba59183` is qualified in the durable
`source-acceptance.json`. Any later decision-documentation commit requires its
own exact-head checks. Inspect the published task ref and require the full
Windows/Linux pair from App `15368`, matching run/artifact provenance, all native
tests with no skips, and package validation before delivery. Preserve current
`dev` fixes and recheck any intervening movement.

```bash
set -euo pipefail
gh api repos/ElectricJack/matter-engine/git/ref/heads/aq/fleet-ridge-94
gh api repos/ElectricJack/matter-engine/git/ref/heads/dev > dev-before.json
aq git merge --project-id matter-engine-cpp --branch-name aq/fleet-ridge-94 \
  --default-branch dev --json
gh api repos/ElectricJack/matter-engine/git/ref/heads/dev > dev-after.json
```

This is the existing authorized protected-dev bootstrap owner action. A scope
refusal stays binding; route execution to authorized LOCAL rather than raw
pushing or resetting refs. Qualify the actual delivered OID and bootstrap bytes,
with full canonical checks or the committed full source-audit aliases. Partial
or failed canonical checks cannot be bypassed through the aliases. Source CI is
not a delivered-head receipt.

## 2. Read current inventory and prepare the two promotion reviews

The paths below are the prepared flow and policy profile on this installation.
The flow SHA256 is
`1760070c181f4c148dbe0aae9e4053d1b2b18a43fef5ed2a41e686f5f1c76634`.
No daemon restart or root retarget is needed for the already-designated `dev`.

```bash
set -euo pipefail
MATTER_PROMOTION_FLOW=/home/jkern/.agent-queue/vault/projects/matter-engine-cpp/policy/promotion/flow.json
aq integration cutover-plan --project matter-engine-cpp \
  --flow "$MATTER_PROMOTION_FLOW" --json > matter-cutover-plan.json
aq integration status matter-engine-cpp --control-only --json
aq policy apply /home/jkern/.agent-queue/policy-profiles/agent-queue-2026-10-11 \
  --project matter-engine-cpp \
  --only playbook:project:promotion-continuous \
  --only playbook:project:promotion-request \
  --set default_branch=dev --set branch_4=staging --set branch_5=main --yes
```

`policy apply` does not support `--json`; the corrected worker attempt was
refused at `policy_diff` scope. The selective owner import submits actual
project-specific reviews and does not approve or activate them. Inspect the
returned revisions and compiled artifact hashes through the authorized decider.
Existing items are not overwritten. Export item checksums are not approved
compiled artifact identities.

## 3. Resolve the live policy contract before changing modes

The supervisor/AQ owner supplies an actual validated, reviewed current policy
file and deployed runtime identity. The local model still requires both `route`
fields; dropping them fails validation, while a dead preflight function's
presence alone does not establish a live requirement. Confirm whether legacy
fields are inert compatibility metadata. Do not invent fields or restore retired
playbooks. A current incompatible runtime requires an authorized AQ resolution.

The following commands become executable only when that input and its live
contract receipt exist. Set `MATTER_GIT_FIRST_POLICY_FILE` to the actual approved
file; preserve both `matter-engine-ci-v1` checks, producer `15368`, hosted CI,
reviewed admission and bounded repair settings. Report material policy changes
for review. Reconcile every live writer/barrier in the fresh cutover inventory
before entering disabled mode. A disabled mode does not force-drain work.

```bash
set -euo pipefail
: "${MATTER_GIT_FIRST_POLICY_FILE:?Set the actual validated current policy file}"
test -f "$MATTER_GIT_FIRST_POLICY_FILE"
MATTER_GIT_FIRST_POLICY_JSON="$(python3 -c 'import json,sys; print(json.dumps(json.load(open(sys.argv[1])), separators=(",", ":")))' "$MATTER_GIT_FIRST_POLICY_FILE")"
matter_generation() {
  aq integration status matter-engine-cpp --control-only --json |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["generation"])'
}
aq project set matter-engine-cpp integration-mode disabled \
  --expected-integration-generation "$(matter_generation)" \
  --reason 'Jack 2026-10-11: prepare git-first cutover with retired playbooks skipped'
aq project set matter-engine-cpp integration-policy "$MATTER_GIT_FIRST_POLICY_JSON" \
  --expected-integration-generation "$(matter_generation)" \
  --reason 'bind reviewed current git-first policy and matter-engine-ci-v1'
aq project set matter-engine-cpp integration-review-mode pull_request \
  --expected-integration-generation "$(matter_generation)" \
  --reason 'retain exact-head reviewed root admission'
aq integration app-verify matter-engine-cpp \
  --policy "$MATTER_GIT_FIRST_POLICY_FILE" --repository-id matter-engine-cpp --json
aq integration app-setup matter-engine-cpp \
  --policy "$MATTER_GIT_FIRST_POLICY_FILE" --repository-id matter-engine-cpp --json
aq promote rulesets --project matter-engine-cpp --file "$MATTER_PROMOTION_FLOW" --json
```

Use the fresh App report's actual fixes and generated rulesets through repository
administration. `app-setup --apply` changes only differing Actions variables; it
does not apply protection. The manifest's attestation App is `5075923`, distinct
from CI App `15368`, and its check version is `matter-engine-ci-v1`. Require the
intended train-mode integration attestation on `dev` and step-specific promotion
attestations on `staging`/`main`, with correct App pins and no bypass. Include
organization/classic protection in the verified effective policy. Do not claim
that a development-mode report establishes train readiness.

## 4. Observe, verify and enable the train and unchanged flow

Proceed only after the current policy, trust variables, effective protection and
review prerequisites pass. Read a fresh generation for every mutation. A stale
CAS refuses; inspect rather than retry against a guessed generation.

```bash
set -euo pipefail
aq project set matter-engine-cpp integration-mode observe \
  --expected-integration-generation "$(matter_generation)" \
  --reason 'observe Jack-approved git-first configuration before publication'
aq integration app-verify matter-engine-cpp --json
aq integration status matter-engine-cpp --json
aq promote validate --project matter-engine-cpp \
  --file "$MATTER_PROMOTION_FLOW" --remote --json
```

After the actual reports pass, enable train mode and activate the unchanged flow:

```bash
set -euo pipefail
aq project set matter-engine-cpp integration-mode train \
  --expected-integration-generation "$(matter_generation)" \
  --reason 'Jack 2026-10-11: enable git-first delivery in code'
aq project set matter-engine-cpp promotion-flow "$MATTER_PROMOTION_FLOW" \
  --expected-integration-generation "$(matter_generation)" \
  --reason 'activate reviewed dev-to-staging-to-main flow; release operator-approved'
```

Flow activation owns create-only staging creation from the actual green `dev`
head. Require no intervening head/check change and return its actual receipt.
Do not manually push staging. Existing `main` is preserved. Initial staging
creation is not first automatic-promotion acceptance.

## 5. Activate only the two approved promotion artifacts

Copy the exact APPROVED compiled artifact hashes from the actual imported
reviews, after their authorized decisions. These variables are intentionally
unset until approval; no template hash is substituted for a compiled artifact.

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
refactor canary acceptance in the migration runbook. Release remains gated by
operator approval; these commands do not approve or initiate a release.
