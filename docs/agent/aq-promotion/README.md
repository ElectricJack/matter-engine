# AQ promotion trust and operator handoff

Review draft for `fleet-ridge-94`, prepared following supervisor message
`msg-716bab7ae7d843469245316071786433` and reported human authorization on
escalation `c9c9fe72`. The authorization covers preparing CI trust inputs.
The project remains in `development` mode; activation and delivery are pending.

Supervisor message `msg-2cb6f0692e0a41af9181581475259d3b` supplied authoritative
identity from the supported `trust-manifest --policy` generator and authorized
preparing [.github/agent-queue-integration.json](../../../.github/agent-queue-integration.json).
Its base canonical SHA256 is
`73bf03ce85880f5fb53800cf9ec64b8c494a462c26d9e2cf7b3e4cc0f6d7efe1`.
The committed-form manifest preserves every generated field and adds only the
reviewed promotion names and source-audit aliases. Its file SHA256 is
`4ba1582f5ef9dc0656d04739034f5891261a05017125fcbb4421adb2667b6f13`.
The attestation App is **`5075923`**, distinct from CI producer **`15368`**.
These identities come from this project's authenticated generation.

The production manifest passes the published AQ trust schema and matches both
policy boundaries and workflow check names. Its presence on this task branch
does not establish delivery, hosted Windows success, review approval or
operator activation. The earlier null-field drafts below remain historical.

The real Windows/Linux workflow is now `tests.yml`, renamed from
`native-windows.yml` with its reusable caller updated. The deployed cutover
preview reads the literal `tests.yml` path and has no path override. The rename
preserves the workflow name, jobs, required check names, runners, commands,
permissions and triggers. After delivery, the operator must regenerate the
preview and resolve its actual blockers; no preview success has been observed.

Supervisor scope extension `msg-77f1a0eef5634947904e6d836731eb76` also requires
automatic task-to-epic-to-`dev` delivery through the git-first train and proof
that a refactor lands without a hand merge. The
[train migration proposal](train-migration.md) records the supported cutover
commands, origin/admission checks, and completion evidence. Its complete
[train-policy.proposal.json](train-policy.proposal.json) uses source-pinned
shared system routes and passes AQ's published value schema. It is awaiting
operator route review, installation/health checks and guarded project binding;
source validation does not establish those approvals.

The complete proposal also supplies valid inputs for the existing supported
`trust-manifest --policy` generator, subject to supervisor/operator review.
This can obtain authoritative identities before the minimal-input deployment.
Neither proposal changes the installed project configuration. Authoritative
full-policy generation has now succeeded without minimal-input deployment.

[hosted-check-policy.json](hosted-check-policy.json) contains the minimal CI
inputs for the published `--hosted-check-policy` bootstrap path: the reviewed
version, ordered platform checks, verified producer and source-audit aliases.
Supervisor message `msg-40c17359a22347fdbbdd268963b75bb0` requested this input
after agent-queue published `d8d4642b4c58a3eda1be2a078be5b4799320b636` / PR1343.
Local `aq integration trust-manifest --help` still lacks the new option at
preparation time. Generation through this alternative minimal-input path awaits
supported CLI/daemon deployment; it is no longer required to resolve this manifest.

[policy.draft.json](policy.draft.json) retains the earlier full
`HierarchicalIntegrationPolicy` proposal with unresolved route references;
it is not required by the minimal bootstrap path.
[trust-manifest.draft.json](trust-manifest.draft.json) shows repository trust,
promotion names, and source-audit checks, with the attestation App unresolved.
The two historical drafts deliberately fail their AQ schemas on required JSON `null`
fields. The CI-only input cannot supply repository or attestation App identity
or activate integration. The production manifest uses the authoritative
identities supplied above, rather than these unresolved draft fields.

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

## Reviewed initial checks

The supervisor accepted **`matter-engine-ci-v1`**, the Windows/Linux check pair,
and the source-audit aliases for this proposal in review message
`msg-a219cd54256c41ea9463b0101a75df39`. Both policy boundaries and the manifest
use the same ordered checks and version. This review does not make the draft an
activatable policy or assert that both hosted checks have passed.

| Required check | Workflow and coverage |
| --- | --- |
| `Native Windows build and tests` | `tests.yml`, self-hosted MSVC build, CPU CTest, distribution/package validation through `tools/ci-windows.ps1` |
| `Linux foundation build and tests` | `tests.yml`, verifier tests, MemoryLib sanitizer tests, MeshChartingLib and AssetStoreLib tests |

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

## Verified CI producer evidence

The supervisor's authenticated GitHub API read of check runs on exact commit
`74e5631273a28d8f0922ad65fb2d9c2f90160cca` verified `app.id = 15368` for both
required checks. Review message `msg-a219cd54256c41ea9463b0101a75df39` authorized
recording that value as a decimal string in both policy boundaries and an
integer in the manifest draft.

| Check | Check-run ID | Observed status/conclusion |
| --- | --- | --- |
| `Linux foundation build and tests` | `114334226391` | Completed / success |
| `Native Windows build and tests` | `114334226524` | Queued / no success yet |

This is evidence for that exact SHA at review time. A later commit needs its
own required checks; the observation does not establish Windows CI success or
identify the AQ attestation App. The supervisor found no applicable project
parent/root integration routes, so their required references remain null.

## Remaining operator gates

| Pending gate | Prepared input | Required authority |
| --- | --- | --- |
| Exact-head hosted CI and delivery | Production manifest and real `tests.yml` workflow | Windows and Linux checks from producer `15368`, configured integration owner |
| Cutover inventory and train readiness | Complete train policy and migration review | User review, fresh supported preview, ready shared routes, guarded project configuration |
| App variables and branch protection | Attestation App `5075923`, version `matter-engine-ci-v1` | Repository administrator and supported App verification |
| Promotion flow and routes | Reviewed staging/release names and source-audit aliases | Local flow activation and reviewed promotion playbook activation |

Policy producer IDs are decimal **strings**; manifest App IDs are positive JSON
**integers**. No App identity is inferred from a fixture, a slug, another
repository or the numeric repository ID. Worker access to `aq playbook list`
was refused with `out_of_scope: list_playbooks`; the supervisor subsequently
confirmed that project listing exposes no applicable routes. The minimal
hosted-check input does not require them. In the retained full-policy draft,
each artifact's `playbook_id` must match its route's ID.
`activation_id` and `compiled_at` are optional nulls; the required artifact
fields remain unresolved.

Admission, repair, cleanup and branchless-parent settings are proposed schema
values. Both boundaries use reviewed admission, no task-specific authorization
list, bounded repair attempts and a human hold on exhaustion. CI is hosted.
This override is a review/generation input; the bound DevelopmentPolicy and
integration mode remain in place. No playbook has been imported or activated.

## Draft validation

The minimal CI input validates offline against `HostedCheckPolicy` from the
exact published agent-queue commit above. The check also confirms that its
ordered names, version, producer and source-audit aliases match the reviewed
policy and manifest drafts. This offline check does not run the new CLI,
deploy the daemon, obtain App identity or generate a production manifest.

An offline check against AQ's current `HierarchicalIntegrationPolicy` and
`IntegrationTrustManifest` models rejected exactly the 16 unresolved policy
route fields and one unresolved manifest attestation App ID, with no other
field errors. It also
confirmed that the workflow job names match the canonical pair, both boundaries
match the manifest's ordered checks/version, and source-audit aliases match the
reusable caller. This verifies draft consistency; it does not establish App
identity, reviewed route authority or hosted Windows CI success.

## Alternative minimal-input generation

After supported deployment, the supervisor/operator uses the read-only
generator with the reviewed minimal hosted-check input. The bound
DevelopmentPolicy remains in place. The generator obtains repository and
attestation App identities from the authenticated binding; no hierarchical
routes need to be synthesized. From the repository root:

```bash
aq integration trust-manifest matter-engine-cpp \
  --repository-id matter-engine-cpp \
  --hosted-check-policy docs/agent/aq-promotion/hosted-check-policy.json \
  --write agent-queue-integration.generated.json --json
```

The published bootstrap generator reads promotion names from the **bound**
flow and accepts named check sets directly from the minimal input, preserving
other valid committed sets. At preparation time the flow is unbound and the
repository has no manifest. Add this proposal's reviewed promotion-name list
to the initial generated copy before review, landing and flow validation; verify
the source-audit set is retained from the minimal input. Otherwise
`attestation_not_in_manifest` remains. Preserve the generator's verified
identities and required-check version. Later runs preserve committed named sets
and obtain promotion names from the activated flow.

Full-policy generation already supplied the authoritative identity. Return
further operator evidence to the worker on `fleet-ridge-94:train-migration`. Repository
administration must set the App and check-version Actions variables. Hosted
Windows CI must pass before delivery is verified. Landing on `dev`, validating
and activating the flow, importing and activating reviewed promotion playbooks,
and observing the first automatic staging promotion remain required; see
[QA cookbook recipe 15](../qa-cookbook.md#15-promotion-ci-and-attestation-audit).
