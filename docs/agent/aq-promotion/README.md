# AQ promotion trust proposal

Review draft for `fleet-ridge-94`, prepared following supervisor message
`msg-716bab7ae7d843469245316071786433` and reported human authorization on
escalation `c9c9fe72`. The authorization covers preparing CI trust inputs.
The project remains in `development` mode; activation and delivery are pending.

[policy.draft.json](policy.draft.json) contains the full
`HierarchicalIntegrationPolicy` structure required by the supported manifest
generator's `--policy` override. [trust-manifest.draft.json](trust-manifest.draft.json)
shows repository trust, promotion names, and the source-audit check set.
Required unresolved fields contain JSON `null`, so the drafts deliberately fail
the AQ policy/manifest schemas. Resolve and review those inputs before passing a
completed policy to the generator or installing the generated manifest at
`.github/agent-queue-integration.json`.

## Verified repository inputs

| Input | Value | Evidence |
| --- | --- | --- |
| GitHub repository | `ElectricJack/matter-engine` | Supervisor's supported repository-access verification |
| Numeric repository ID | `1266132968` | Same verification; this identifies the repository |
| Default/delivery branch | `dev` | Same verification and AQ integration status |
| AQ canonical repository ID | `matter-engine-cpp` | `aq integration status matter-engine-cpp --control-only --json` |
| Integration mode | `development` | Same status, generation `13` at preparation time |

The repository ID does not identify either App. The operator must read the
current generation before any later guarded change.

## Proposed initial checks

The proposed version is **`matter-engine-ci-v1`**, subject to review. Both policy
boundaries and the manifest use the same ordered checks and version. Version
approval does not assert that hosted CI has passed.

| Required check | Workflow and coverage |
| --- | --- |
| `Native Windows build and tests` | `native-windows.yml`, self-hosted MSVC build, CPU CTest, distribution/package validation through `tools/ci-windows.ps1` |
| `Linux foundation build and tests` | `native-windows.yml`, verifier tests, MemoryLib sanitizer tests, MeshChartingLib and AssetStoreLib tests |

`check_sets.promotion-source-audit` contains:

```text
unattested-ci / Native Windows build and tests
unattested-ci / Linux foundation build and tests
```

These match the reusable-workflow caller `unattested-ci` in
`main-attestation.yml`. AQ's source-admission fallback accepts them when canonical
checks are absent on an unattested delivery push. It retains both platform gates
and refuses failed or partially present canonical evidence. The prepared flow's
`gate.checks: inherit` selects the canonical pair for both promotion steps.

The draft permits `Agent Queue Promotion Attestation (staging)` for continuous
`dev -> staging` and `Agent Queue Promotion Attestation (release)` for
operator-approved `staging -> main`. The daemon App publishes the proofs; the
workflow verifies them.

## Inputs still required

| Unresolved input | Draft fields | Required authority |
| --- | --- | --- |
| Numeric CI producer App ID | Both policy `required_checks.producer_id` fields and manifest `ci_producer_app_id` | Supported verification of real repository check runs: exact names, SHA, `app.id`, check-run and suite IDs; the same producer on both boundaries |
| Numeric AQ attestation App ID | Manifest `attestation_app_id` | Supported daemon/App identity and generator output; distinct from the CI producer |
| Parent/root routing snapshots | Each `route.playbook_id` and required `artifact` fields | Supervisor-selected, reviewed playbook references; IDs and hashes must belong to the selected project or system routes |
| Check-set version approval | Both boundaries and manifest `required_checks.version` | Review of proposed `matter-engine-ci-v1` and both workflow jobs |

Policy producer IDs are decimal **strings**; manifest App IDs are positive JSON
**integers**. No App identity is inferred from a fixture, a slug, another
repository or the numeric repository ID. Worker access to `aq playbook list`
was refused with `out_of_scope: list_playbooks`, leaving routing snapshots for
supervisor review. Each artifact's `playbook_id` must match its route's ID.
`activation_id` and `compiled_at` are optional nulls; the required artifact
fields remain unresolved.

Admission, repair, cleanup and branchless-parent settings are proposed schema
values. Both boundaries use reviewed admission, no task-specific authorization
list, bounded repair attempts and a human hold on exhaustion. CI is hosted.
This override is a review/generation input; the bound DevelopmentPolicy and
integration mode remain in place. No playbook has been imported or activated.

## Draft validation

An offline check against AQ's current `HierarchicalIntegrationPolicy` and
`IntegrationTrustManifest` models rejected exactly the 18 unresolved policy
fields and two unresolved manifest App IDs, with no other field errors. It also
confirmed that the workflow job names match the canonical pair, both boundaries
match the manifest's ordered checks/version, and source-audit aliases match the
reusable caller. This verifies draft consistency; it does not establish App
identity, reviewed route authority or hosted Windows CI success.

## Operator handoff

After resolving and reviewing the override, the supervisor/operator uses the
read-only generator with `--policy` pointing to the completed file. It obtains
repository and attestation App identities from the authenticated binding.
For example, with a reviewed resolved policy in the current directory:

```bash
aq integration trust-manifest matter-engine-cpp \
  --repository-id matter-engine-cpp \
  --policy policy.reviewed.json \
  --write agent-queue-integration.generated.json --json
```

The current generator reads promotion names from the **bound** flow and named
check sets from a valid **committed** manifest. At preparation time the flow is
unbound and the repository has no manifest. Add this proposal's reviewed
promotion-name list and source-audit check set to the initial generated copy
before review, landing and flow validation. Otherwise
`attestation_not_in_manifest` remains. Preserve the generator's verified
identities and required-check version. Later runs preserve committed named sets
and obtain promotion names from the activated flow.

Return the authoritative generated output and reviewed route/version disposition
to the worker on `fleet-ridge-94:authoritative-manifest`. Repository
administration must set the App and check-version Actions variables. Hosted
Windows CI must pass before delivery is verified. Landing on `dev`, validating
and activating the flow, importing and activating reviewed promotion playbooks,
and observing the first automatic staging promotion remain required; see
[QA cookbook recipe 15](../qa-cookbook.md#15-promotion-ci-and-attestation-audit).
