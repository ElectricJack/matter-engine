# Matter Engine git-first train migration

Proposal for `fleet-ridge-94`, following supervisor scope extension
`msg-77f1a0eef5634947904e6d836731eb76`. Restore automatic task-branch delivery
through epic branches into `dev`, retain the reviewed CI trust bootstrap, and
prove both a refactor delivery and the first automatic `dev -> staging`
promotion. `staging -> main` retains operator approval.

The [2026-10-11 decision](decision-2026-10-11.md) permanently skips
`parent-integration` and `root-train` import and activation for this cutover.
Git-first delivery runs in code. Only the two project promotion playbooks remain
in the activation plan. This replaces the earlier route hold and route-readiness
requirements; the exact CI, App, review, CAS and release gates still apply.

## Current binding direction

Owner direction `msg-43295073d3df42deaae6dfffb147ffe1` (sequence `10086`) and
supervisor relay `msg-6c53372280de447dafd12a6602025f16` (sequence `10088`)
supersede the earlier cutover/merge instructions. AQ PR1358 is reported deployed
at `73509417ffeb31c197ac7419d3aa5491d196eec6`. The supplied route-free
[git-first-policy.json](git-first-policy.json) is an exact mirror of the owner
file, SHA256 `e7d01545c033b00d573b74d920ec9781d2ac7e4cce95b3f6fd24aca736362cdf`.
Jack requires AQ's live policy with Matter's check set: parent admission
`reviewed`, root admission `authorized`, both repair exhaustion settings
`continue`, no route/artifact identities and no authored CI block (hosted).
This replaces the historical proposal's reviewed-root/human-hold settings.

Use the corrected [operator handoff](operator-cutover-2026-10-11.md). Do not use
`integration cutover` or `cutover-plan`; the root already targets `dev`. The
owner reports GitHub default `dev` and Actions variables already configured.
Generate/reconcile the repository bootstrap through the authenticated owner,
publish the required code on the held branch, then deliver through the current
development owner. A doc-only update is not the bootstrap action. Exact-head CI,
App identity/protection, actual delivery, promotion reviews and operator release
approval remain required.

## Historical evidence and proposed inputs

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

[train-policy.proposal.json](train-policy.proposal.json) is the historical
`HierarchicalIntegrationPolicy`, validated offline against published AQ commit
`b2bb25872e769cbc5013e8b5a5ea62803fbc721c`. Its file SHA256 is
`cbdf69cdfc043fd929a92e31dbfc192a26127fc4de8c5442d6a565e26a7b69d7`.
It keeps the reviewed Windows/Linux `matter-engine-ci-v1` set, producer
`"15368"`, reviewed admission with empty task authorization lists, hosted CI,
bounded repairs ending in a human hold, and default cleanup. Root cadence is
300 seconds, capped at 1800; epic collection has no settling delay. It is not a
current binding input: its schema-required retired route snapshots need
reconciliation with the supported live git-first policy contract.
`prerequisite_branches: wait-for-parent` makes the ordinary non-agent-queue
project default explicit. This proposal does not change assignment routing.

The historical routes were the published **system-scoped** shared bundles, not
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
supervisor previously found no applicable project routes. Jack has now retired
these import/activation requirements. Preserve these historical identities as
evidence; do not substitute other bundles or fabricate new route snapshots.

## Authoritative manifest and preview refusal

Supervisor reply `msg-2cb6f0692e0a41af9181581475259d3b` records successful
authenticated generation using this full policy, `--repository-id
matter-engine-cpp --print --json`. The generated base canonical SHA256 is
`73bf03ce85880f5fb53800cf9ec64b8c494a462c26d9e2cf7b3e4cc0f6d7efe1`.
Its verified fields are schema `aq.integration-trust.v1`, canonical repository
`matter-engine-cpp`, GitHub repository `1266132968`, full name
`ElectricJack/matter-engine`, CI App `15368`, attestation App `5075923`,
integration attestation name, and the ordered Windows/Linux `matter-engine-ci-v1`
checks. Reconstructing those exact fields matches the supplied canonical hash.
The production manifest adds only the already-reviewed promotion names and
source-audit check set. Its file SHA256 is
`4ba1582f5ef9dc0656d04739034f5891261a05017125fcbb4421adb2667b6f13`.

The same reply records a refused cutover preview at `dev`
`81166c7bd29a8126026a5b9a2f0bec95c9d9f744`: `git show` could not read
`.github/workflows/tests.yml`. The manifest was also absent there. Published
AQ `src/integration/cutover.py`, `CutoverGit.observe`, reads the literal
`tests.yml` path and extracts explicit `pull_request.branches`; its command
contract has no workflow-path override. `promotion_workflow_triggers` in
`promotion_steps.py` emits the same path. This is a repository-specific
filename assumption, not evidence that Matter Engine lacks the platform jobs.

The supported repository preparation renames the **real** Windows/Linux
workflow from `native-windows.yml` to `tests.yml` and updates the attestation
caller's `uses` path. The workflow bytes, name, jobs, check names, permissions,
runners and triggers are preserved. Its explicit PR list already includes
`dev`, `staging`, and `main`, and its `aq/**` push pattern covers candidate and
promotion branches. No placeholder workflow is added. Retaining the former
filename would instead require a separately reviewed and deployed AQ change
to discover/configure the real check-producing workflow; no current flag
provides that behavior.

Delivery of the manifest and actual workflow is still required. The old
`cutover-plan` diagnostic explains the historical rename; current owner direction
expressly forbids rerunning that branch-retarget procedure. Use the supplied
route-free policy and fresh status/readiness receipts instead. The earlier
DevelopmentPolicy finding is historical and does not qualify the train policy.

## Trust and route preparation

1. The supervisor/operator reviews this proposal and selects a refactor canary
   with a known source head, completion identity and epic branch. Preserve a
   pre-cutover status snapshot, live writers, owners, subjects and open batches.
   Existing subjects retain their frozen artifacts; resolve their named blockers
   through supported controls. The old aborted `smart-dune` batch is history,
   not permission to resume or edit it.
2. Validate the supplied route-free policy against current AQ. The deployed
   PR1358 contract makes routes optional and omits them from snapshots. Local
   validation at the reported deployed revision passes with the complete Matter
   CI pair, producer `15368`, parent reviewed/root authorized admission and repair
   exhaustion `continue`. Preserve the owner file's exact hash. The historical
   route-bearing policy is not a binding input.
3. The authenticated owner runs the exact supplied-policy trust generator in
   [operator stage 1](operator-cutover-2026-10-11.md#1-generate-and-deliver-the-repository-bootstrap).
   The worker's attempt was refused at `integration_trust_manifest`, exit `4`,
   before writing. Do not retry or bypass that refusal. Supervisor generation
   subsequently succeeded in `msg-dbe933c0d0654205929dfe4dcb486426` (sequence
   `10096`), base SHA256 `73bf03ce85880f5fb53800cf9ec64b8c494a462c26d9e2cf7b3e4cc0f6d7efe1`.
   [Reconciliation](evidence/git-first-manifest-reconciliation.json) preserves
   generated identities with the already-reviewed promotion names and source-audit aliases;
   the unbound-flow generator cannot supply its future promotion names. Report
   any resulting readiness/ordering conflict before train activation.
4. Commit and publish the authoritative manifest with this task's workflows,
   verifier and license, including the real workflow's rename to `tests.yml`,
   then deliver them to `dev` through the configured
   development owner. The earlier bootstrap merge handoff is superseded; a
   worker does not merge into `dev`. Verify the exact delivered SHA
   and Windows/Linux checks; a queued Windows job is still pending. The
   self-hosted `matter-engine-msvc` runner must be online and able to finish.
   Current source CI success on another SHA does not satisfy this gate.
5. Ensure the selected epic/candidate's own tree carries the `aq/**` push
   workflow and verifier. Updating `dev` does not update an older epic tree.
   The integration owner performs any necessary exact-head refresh; the worker
   never edits an unheld task's branch.

## Windows CI runner blocker and operator setup

Supervisor message `msg-a9dd9f7c77be481891bf8327517b7311` independently verified
the production manifest and unchanged workflow rename on exact source
`a2186ec3f352426c0f79dcd2a24a6745b657ca96`. Both check runs have producer
App `15368`:

| Required check | Run ID | Observed state |
| --- | --- | --- |
| `Linux foundation build and tests` | `114342288807` | Completed / success |
| `Native Windows build and tests` | `114342288627` | Queued / no conclusion |

The authenticated repository `actions/runners` request succeeded and returned
no runners. Windows runner availability is a concrete external blocker.
`dev` remains `81166c7bd29a8126026a5b9a2f0bec95c9d9f744`; no landing or
activation was performed. These are observations for that source SHA only.
Later commits require their own check evidence. Do not repeat the cutover
preview against unchanged `dev`, remove the Windows gate, relabel an unsuitable
host or treat the queued run as success.

The repository owner/operator selects an authorized existing Windows machine
and sets up its runner. This worker supplies instructions; it does not provision
another host or perform runner registration. The real job in
`tests.yml` requires **all** labels `self-hosted`, `Windows`, `X64`, and
`matter-engine-msvc`, runs native `powershell`, allows 90 minutes, and sets
CMake build parallelism to four and CTest parallelism to two.

The checked-in resolver and build graph require these installed inputs:

| Input | Requirement in this source tree |
| --- | --- |
| Host and workspace | Windows x64, native PowerShell, Git for Windows on PATH, writable local Windows drive; UNC checkout roots are rejected |
| Visual Studio | **Visual Studio 2022 Community**, locatable through `vswhere`, with `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`; other VS product IDs are rejected |
| Compiler | MSVC v143 tools **`14.44.35207`**, including `Hostx64/x64/cl.exe` |
| Windows SDK | **`10.0.26100.0`**, installed headers and UM/UCRT x64 libraries |
| Build tools | Visual Studio-bundled CMake and Ninja at the resolver's paths; root CMake requires at least 3.25 |
| Python | Native Windows **`3.13.14`**, discoverable by the actual runner account via `py.exe -3.13` or `python.exe`; CMake rejects other patch versions |
| Vulkan SDK | **`1.4.357.0`** at `C:\VulkanSDK\1.4.357.0`, including headers, `vulkan-1.lib`, validation-layer manifest and `Bin\glslc.exe` |

These pins come from `tools/windows/MatterWindowsToolchain.psm1`,
`tools/build-windows.ps1`, `CMakeLists.txt` and `tools/ci-windows.ps1`.
The wrapper enters `VsDevCmd.bat` itself. Install dependencies so that the
chosen service account can discover/read them and write its workspace.

Operator procedure:

1. Before starting the listener, from a separate native checkout of the intended
   source under the actual runner account, verify installed tools and paths:

   ```powershell
   & ./tools/build-windows.ps1 -Config RelWithDebInfo -PreflightOnly
   ```

   Preflight resolves the toolchain; it does not run the build or hosted CI.
2. In `ElectricJack/matter-engine`, open **Settings → Actions → Runners →
   New self-hosted runner**, select **Windows / x64**, and use GitHub's current
   download, extraction and registration instructions on the selected host.
   Use a runner directory such as `C:\actions-runner`, outside AQ worktree
   slots. Obtain the short-lived registration token locally through this
   owner/admin surface; do not put it in the repository or AQ messages.
   GitHub's [runner setup instructions](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/add-runners)
   describe this supported path.
3. Preserve the automatically assigned OS/architecture/default labels and add
   `matter-engine-msvc` during registration with `--labels matter-engine-msvc`,
   or through that runner's label editor. Confirm all four required labels on
   the registered host, per [GitHub's label instructions](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/apply-labels).
4. For a persistent Windows runner, select service installation during
   `config.cmd` in an administrator shell. Select the account that has the
   pinned tools and local workspace access; verify its service in Windows
   Services or with `Get-Service 'actions.runner.*'`. Alternatively run the
   registered runner interactively with `run.cmd` under that account while
   it remains available. Windows service setup is part of registration,
   per [GitHub's service instructions](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/configure-the-application).
5. Verify **online / idle** status and repository access, with outbound HTTPS
   on port 443 to GitHub's documented runner, action-download and artifact
   endpoints. A matching online runner can accept a queued job; registration
   alone does not establish availability. See [GitHub's runner requirements](https://docs.github.com/en/actions/reference/runners/self-hosted-runners).
6. The supervisor returns the authorized runner's name, labels, online state,
   service-account preflight result, and completed latest-source Windows and
   Linux check IDs/conclusions. If the queued run expires or fails, the
   repository owner reruns that failed workflow through GitHub's supported
   Actions UI after fixing availability. Delivery and a fresh cutover preview
   follow passing exact-head checks and the existing review/authority gates.

The actual workflow runs `./tools/ci-windows.ps1`, which builds, runs CTest,
builds `matter_dist`, and validates the distribution. It uploads `artifacts/ci/`
containing `native-windows.log`, `toolchain.json`, and `ctest.xml` under
`native-windows-<github.sha>-<run_attempt>`. A local preflight or build does
not substitute for the exact-head hosted check from App `15368`.

## Bind and activate the project

The corrected [operator handoff](operator-cutover-2026-10-11.md) supplies exact
remaining commands and the concrete policy/hash. LOCAL or the live named
supervisor runs the mode/policy/flow mutations with a fresh generation for each
CAS. The worker prepares, validates and publishes repository code; it neither
writes the default branch nor bypasses scope refusals.

The required owner sequence after actual bootstrap delivery is `disabled ->
integration-policy <supplied file> -> observe -> app-verify -> train ->
promotion-flow <unchanged file>`. Then selectively import, obtain authorized
review decisions and activate only `promotion-continuous` and
`promotion-request`. Flow activation owns create-only staging creation from the
current green `dev` head. Preserve an existing `main`; release remains
operator-approved. No daemon restart, retarget, legacy cutover command or
retired route is part of this sequence.

Reconcile actual live writers/subjects/batches before changing modes. Disabled
mode does not force-drain work or replace frozen snapshots. Require actual
intended-mode App readiness, generated identity, full check semantics, effective
protection and Actions variables before train publication. A development-mode
report is not that proof. Admission follows the supplied current policy,
including its explicitly authorized root setting; do not reapply the historical
proposal's reviewed-root requirement.

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
   then permit root admission under the supplied authorized policy. An
   incomplete or nested epic is not a completed root source; do not restore the
   historical reviewed-root PR requirement as the current admission setting.
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
  Actions variables, valid protection, and the validated live git-first policy.
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
