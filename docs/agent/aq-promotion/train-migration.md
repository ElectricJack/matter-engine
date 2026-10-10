# Matter Engine git-first train migration

Proposal for `fleet-ridge-94`, following supervisor scope extension
`msg-77f1a0eef5634947904e6d836731eb76`. Restore automatic task-branch delivery
through epic branches into `dev`, retain the reviewed CI trust bootstrap, and
prove both a refactor delivery and the first automatic `dev -> staging`
promotion. `staging -> main` retains operator approval.

## Current evidence and proposed inputs

Supported `aq integration status matter-engine-cpp --control-only --json`
reports `projection_kind: train`, effective/desired mode `development`,
generation `13`, repository `matter-engine-cpp`, and `dev` at
`81166c7bd29a8126026a5b9a2f0bec95c9d9f744`. Its observed `dev` visit is idle
with zero repository/routed candidate IDs. These are preparation observations,
not current values to reuse for mutations.

The active git-first selector already visits development projects. Retiring
`development_runtime` does not by itself prove why a completion is missing:
`DatabaseTargets` includes development, while `_pending_tasks` requires a
completed task with a live branch origin in the designated repository. The
operator must check the selected canary's origin, completion, parent target and
admission. Changing the project mode alone does not restore missing origins.

[train-policy.proposal.json](train-policy.proposal.json) is a complete
`HierarchicalIntegrationPolicy`, validated offline against published AQ commit
`b2bb25872e769cbc5013e8b5a5ea62803fbc721c`. Its file SHA256 is
`cbdf69cdfc043fd929a92e31dbfc192a26127fc4de8c5442d6a565e26a7b69d7`.
It keeps the reviewed Windows/Linux `matter-engine-ci-v1` set, producer
`"15368"`, reviewed admission with empty task authorization lists, hosted CI,
bounded repairs ending in a human hold, and default cleanup. Root cadence is
300 seconds, capped at 1800; epic collection has no settling delay.
`prerequisite_branches: wait-for-parent` makes the ordinary non-agent-queue
project default explicit. This proposal does not change assignment routing.

The proposed routes are the published **system-scoped** shared bundles, not
agent-queue's project-scoped routes. Both use an empty scope identifier. Full
artifact snapshots, including schema generation, source digest, command
fingerprint, compiler and version, are in the policy.

| Boundary | Shared playbook | Exact artifact SHA256 |
| --- | --- | --- |
| Parent / epic | `parent-integration` | `a7455b7c3a2d2bdca62365e6d1d7f4ebe158a47aa0db5009812b8294349ddacd` |
| Root / dev | `root-train` | `6e28d8dbc4eb93c255813417c6b5d03fdda5bf099dd1be70cf039fb67aa2ed52` |

| Playbook | Normalized source SHA256 | Command contract SHA256 |
| --- | --- | --- |
| `parent-integration` | `7dcad46a111fc4f0c9eae94ca3b57ed44252212c236f934aafa2957e7d4de13c` | `a4b6e09f12c8c3111b1987f4d09fd86c45b973a1cc83b9d689f566d4e937b4c0` |
| `root-train` | `37a6a9659c6cbe46244f1e45a25987bbe47669399bdf33691f924a16933f41d6` | `33581303bf8556083f103154d1b94b810df3c398a49c2dbe90bcf68d37697e41` |

These identities come from that commit's `src/prompts/reviewed_playbooks/`.
Artifact bytes match `artifact.sha256` and review-manifest digests; computed
command fingerprints match the manifests; published diagnostics are empty.
This source check does not prove installation, current contract validity,
activation health or operator approval of this project's cutover. The
supervisor previously found no applicable project routes. If different bundles
are selected, revise and revalidate the policy before binding it.

## Trust and route preparation

1. The supervisor/operator reviews this proposal and selects a refactor canary
   with a known source head, completion identity and epic branch. Preserve a
   pre-cutover status snapshot, live writers, owners, subjects and open batches.
   Existing subjects retain their frozen artifacts; resolve their named blockers
   through supported controls. The old aborted `smart-dune` batch is history,
   not permission to resume or edit it.
2. The local operator validates and imports the shared bundles from supported
   deployed packages, then activates the reviewed exact digests. Validation must
   report zero errors/questions and activation health must report the matching
   enabled, ready artifacts. A contract mismatch requires reviewed replacement
   artifacts and a revised policy. Do not use historical runbook hashes or
   fabricate activation IDs.

   ```bash
   aq playbook v2-validate --path reviewed-playbooks/parent-integration/artifact.json
   aq playbook v2-validate --path reviewed-playbooks/root-train/artifact.json
   aq playbook v2-import --path reviewed-playbooks/parent-integration
   aq playbook v2-import --path reviewed-playbooks/root-train
   aq playbook activate --playbook-id parent-integration --artifact-sha256 sha256:a7455b7c3a2d2bdca62365e6d1d7f4ebe158a47aa0db5009812b8294349ddacd --enabled
   aq playbook activate --playbook-id root-train --artifact-sha256 sha256:6e28d8dbc4eb93c255813417c6b5d03fdda5bf099dd1be70cf039fb67aa2ed52 --enabled
   aq playbook activation-health --playbook-id parent-integration
   aq playbook activation-health --playbook-id root-train
   ```

3. The supervisor/operator can now use the already-supported, read-only full
   policy generator without binding the policy or changing modes:

   ```bash
   aq integration trust-manifest matter-engine-cpp \
     --repository-id matter-engine-cpp \
     --policy docs/agent/aq-promotion/train-policy.proposal.json \
     --write agent-queue-integration.generated.json --json
   ```

   This resolves the repository and attestation App through the authenticated
   daemon. The reviewed minimal `--hosted-check-policy` path remains an
   alternative after its supported deployment. Before the flow is bound and a
   manifest committed, the full-policy generator supplies neither the reviewed
   promotion names nor the source-audit aliases: add those exact fields from
   `trust-manifest.draft.json` / `hosted-check-policy.json` to its output for
   review. Preserve all generated identities and the required-check version.
   Never copy the draft's unresolved App field into production.
4. Commit and publish the authoritative manifest with this task's workflows,
   verifier and license, then deliver them to `dev` through the configured
   owner. During bootstrap the supervisor owns the expressly authorized hand
   merges. A worker does not merge into `dev`. Verify the exact delivered SHA
   and Windows/Linux checks; a queued Windows job is still pending. The
   self-hosted `matter-engine-msvc` runner must be online and able to finish.
   Current source CI success on another SHA does not satisfy this gate.
5. Ensure the selected epic/candidate's own tree carries the `aq/**` push
   workflow and verifier. Updating `dev` does not update an older epic tree.
   The integration owner performs any necessary exact-head refresh; the worker
   never edits an unheld task's branch.

## Bind and activate the project

The worker prepares and verifies repository artifacts. LOCAL or a live named
project supervisor performs the integration configuration; shared bundle
import/activation and repository administration remain operator actions.
No daemon restart is indicated by the existing train projection. Workers never
edit global daemon configuration or restart it.

At the observed quiet point, the authorized supervisor/operator performs these
guarded changes with a **newly read** integration generation for each command:

Before mutation, the local operator records the supported cutover inventory:

```bash
aq integration cutover-plan --project matter-engine-cpp \
  --flow "$HOME/.agent-queue/vault/projects/matter-engine-cpp/policy/promotion/flow.json" \
  --json
```

Return its exact repository/default OID, generation, PR/owner/subject inventory
and refusals to the held worker for review. This preview is LOCAL-only; the
worker does not execute it or manufacture its result. The root is already
`dev`, so this proposal does not apply a branch-retarget cutover. A refusal for
live work remains evidence to reconcile through supported controls; preview
does not authorize ignoring a barrier or enabling another publisher.

```bash
aq integration status matter-engine-cpp --control-only --json
aq project set matter-engine-cpp integration-mode disabled \
  --expected-integration-generation <fresh-generation> \
  --reason 'prepare reviewed Matter Engine git-first train policy'
aq project set matter-engine-cpp integration-policy <policy-json> \
  --expected-integration-generation <fresh-generation> \
  --reason 'bind reviewed shared routes and matter-engine-ci-v1'
aq project set matter-engine-cpp integration-review-mode pull_request \
  --expected-integration-generation <fresh-generation> \
  --reason 'require exact-head PR admission for Matter Engine roots'
```

`<policy-json>` is the complete, verified proposal file encoded as one JSON
argument. Keep the existing repository/default-branch binding; both already
name `matter-engine-cpp` / `dev`. Do not switch modes while a repair writer is
publishing. `disabled` prevents new target visits; it does not erase existing
subjects or grant a forced drain. Do not bind the hierarchical policy while
still in development: that mode parses policy input as `DevelopmentPolicy`.

The repository administrator sets `AQ_INTEGRATION_ATTESTATION_APP_ID` from the
authoritative output and `AQ_INTEGRATION_REQUIRED_CHECK_VERSION` to
`matter-engine-ci-v1`. While publication is quiet, configure effective `dev`
protection to require the integration attestation pinned to that App without
bypass. Use the exact fixes / expected ruleset returned by supported
`app-verify` / `app-setup`; inspect organization and classic rules too. Do not
drop either platform check or accept a different producer to make setup green.

```bash
aq integration app-verify matter-engine-cpp \
  --policy docs/agent/aq-promotion/train-policy.proposal.json \
  --repository-id matter-engine-cpp --json
aq project set matter-engine-cpp integration-mode observe \
  --expected-integration-generation <fresh-generation> \
  --reason 'observe reviewed Matter Engine train configuration'
aq integration app-verify matter-engine-cpp --json
aq integration status matter-engine-cpp --json
```

Require `app-verify` readiness and no failing trust/protection/variable items,
matching route health, and concrete canary origin/completion evidence before
enabling publication. The development-mode protection report alone does not
prove strict train readiness. Review mode remains `pull_request`; reviewed
root admission needs an open PR, passing checks and approval of the exact
source head, with no outstanding changes requested.

```bash
aq project set matter-engine-cpp integration-mode train \
  --expected-integration-generation <fresh-generation> \
  --reason 'activate reviewed Matter Engine branch-to-epic-to-dev delivery'
aq integration status matter-engine-cpp --control-only --json
```

Expect effective/desired `train`, the reviewed policy generation, and a new
`dev` target visit with kind `root`. Status may retain older historical target
observations. Do not substitute `integration enable`, `develop`,
`onboard-train`, legacy rollback controls, manual database edits or token
changes for the currently supported commands. The new policy does not replace
existing frozen subject pins.

## Completion evidence

The supervisor names the refactor canary. Before observing it, record its exact
source SHA, completion identity, epic head, `dev` head and selected origin.
A completion already hand-merged into `dev` cannot prove automatic delivery.
For an older task missing a canonical origin or PR, report the blocker and use
only applicable previewed recovery controls; never invent completion/origin
rows. Operator recovery itself is not an autonomous canary result.

Supervisor reply `msg-01ad964706924027b469c5bc07219817` selects
`clear-crest-48.1.2`, then in progress on `aq/clear-crest-48.1.2` under
`clear-crest-48.1`, which is under `clear-crest-48`. The expected sequence is:

1. The held canary worker completes and checkpoints its own source. The train
   proves that source's origin and collects it into the immediate parent's
   verified branch target. It does not skip `clear-crest-48.1` and merge the
   leaf directly into `dev`.
2. `clear-crest-48.1` completes normally only after required children are
   completed and Git proves their current sources in its aggregate. Nested
   child readiness, holds, exact aggregate-head checks and required tree
   reviews must all be satisfied. The recorded nested completion must identify
   the aggregate head just evaluated, rather than an earlier parent close.
3. The completed `clear-crest-48.1` aggregate enters its own parent target,
   `clear-crest-48`. That root epic has the same required-child, current-head,
   hold and review requirements. Its ordinary completion and verified aggregate
   then permit a root PR; the PR service refuses an incomplete epic or a nested
   epic as a root source. Root admission still requires the exact-head PR gate.
4. The root train publishes the tested `clear-crest-48` aggregate into `dev`.
   Record the canary's source containment through both actual epic refs and
   `dev`, plus each train publication's provenance and expected old/new OIDs.

The supervisor supplies actual parent refs/origins and required-child state;
task names alone do not prove branch identity. A leaf's passing close or its
first parent merge does not establish delivery into `dev`. Do not force-close,
waive, reparent or abandon open ancestor work to produce the result. If this
canary lands manually before activation, the supervisor selects the next
eligible existing refactor. If its ancestors remain open, record that concrete
prerequisite and keep this task incomplete while their normal work proceeds.

Keep `fleet-ridge-94` held until all of these are recorded:

- Authoritative manifest and workflows on `dev`, matching App/check-version
  Actions variables, valid protection, and ready reviewed route artifacts.
- A train-owned epic batch naming the canary's exact source, its candidate SHA,
  both required checks from producer `15368`, and recorded publication into the
  epic, through each nested ancestor. Then a train-owned root batch and delivery
  proof into `dev`, including
  its expected old/new target OIDs and integration attestation. Confirm Git
  contains the canary source on both delivered targets. Distinguish PR approval
  from publication: the daemon, rather than a hand merge, must publish.
- The prepared promotion flow validated and activated by the local operator,
  and both reviewed project promotion playbooks imported and activated as in
  [QA recipe 15](../qa-cookbook.md#15-promotion-ci-and-attestation-audit).
- `aq promote status --project matter-engine-cpp --step staging` proving the
  first automatic promotion's exact source/target SHAs and staging attestation.
  Release still requires operator approval; this task does not request one.

Use one durable wait on `fleet-ridge-94:train-migration` or the returned job/task
condition while external activation or delivery proceeds. Consume its result
once and inspect the actual outcome; neither a satisfied wait nor a passing
task close proves Git delivery. Report failed gates and pending Windows CI
truthfully, without polling unchanged state.

## Source references

The policy/value schema and supported configuration behavior were inspected
in published AQ commit `b2bb25872e769cbc5013e8b5a5ea62803fbc721c`:
`src/integration/models.py`, `records.py` (`PolicyActivation.configure`),
`train_sources.py` (`DatabaseTargets`, `_pending_tasks`),
`epics.py` (`EpicGraph.target`, `EpicReadinessEvaluator.evaluate`),
`root_pull_requests.py` (`EpicPullRequestService`),
`src/commands/project_commands.py`, `integration_commands.py`, and
`src/playbooks/definition.py`. The command forms match local `--help`.
The current Subject integration guide and git-first train runbook describe
operation and frozen pins; onboarding/migration docs marked historical are
context, not current CLI instructions.
