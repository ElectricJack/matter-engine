# Component separation baseline

Task `clear-crest-48.1.1`, 2026-10-10. This directory pins the extraction
starting point and fixes component ownership. It supplies documentation to the
contract gate; it does **not** demonstrate extracted libraries, portable recipe
validation, WASM equivalence, graphics independence, or performance acceptance.

Start with these documents:

- [Design source](2026-10-10-matter-engine-component-separation-design.md)
  and [contracts v1 source](2026-10-10-matter-component-contracts-v1.md).
- [Ownership](ownership.md): fixed module names, dependencies and central-file writers.
- [Source and capability inventory](inventory.md): current implementations,
  coupling and native compatibility obligations.
- [Fixture manifest](fixtures.json): source inputs, assertions, focused commands
  and explicit gaps for the contract/extraction gates.
- [Validation and benchmark profiles](validation.md): existing commands,
  reserved future targets and measurement protocol.
- [Pinned baseline](baseline.json): exact commits, source hashes, reconciliation,
  pre-existing failures and this task's checks.
- [Vertical-slice qualification, 2026-10-20](qualification-2026-10-20.md):
  native/browser parity, viewer and matched-view results, measurements,
  capabilities and declared gaps (task `clear-crest-48.9.2`).
- [Component packages: consumers and migration](consumers.md): local
  redistributable packages, their notices, host seams, native-only features
  and migration from the engine (task `clear-crest-48.9.6`).
- [Legacy forwarder retirement, 2026-10-20](retirement-2026-10-20.md): each
  removed `.cpp` build-link forwarder, its canonical replacement, and the
  compatibility adapters kept (task `clear-crest-48.9.7`).

The live starting commit is `98af23851dd3424ddbfc62624cd2afb850e9d09d`, the
observed `origin/dev` head. The design inspected
`d006f20e91e4193d9b6e5a3dd2a165e2c3171143`. These are different baselines:
272 paths changed, including static-default integration, restored mountain
population, GI/OBJ export, VT and geometry reliability, and object evaluation.
Use the live commit for extraction; retain the older commit as design provenance.
Local `main` and `vg-vt-improvements` refs are stale and are recorded separately.
No source branch was reset, merged, or rewritten by this task.

## Provenance and authority

The two source documents are byte-for-byte copies, supplied by the project
supervisor through authorized AQ messages after the worker's vault read was
refused. Their full vault paths, hashes and message IDs are in `baseline.json`.
The design retains `status: draft`; contracts retain
`status: implementation_baseline`. Copying them changes neither approval nor
implementation status. The task/contract gate supplies implementation authority.

References inside those copies use the original vault-relative layout. The two
specs resolve to one another here; references to older proposals and the vault
implementation plan remain vault references, not missing repository deliverables.
The documents in this directory reconcile the current checkout without editing
the source copies or overruling the contract specification.

The neutral contracts and public-consumer/fake-service gate must be **delivered**
before parallel extraction starts. This baseline documentation alone cannot
release that gate. Modules prepare adapters within their owned directories;
ordered integration tasks own existing central entrypoints and source manifests.
Retirement follows native compatibility and the portability matrix.

## Recheck this record

From the repository root:

```sh
python3 docs/component-separation/verify.py
git diff --check
```

The verifier checks spec-copy hashes, fixture paths and source hashes **at the
pinned commit**, native target/CTest registration and unique IDs. It does not
run generators, allocate GPU resources, or treat an existing Matter archive as
proof of an independent component build. Later code changes should add evidence
against their own commits while preserving this starting record.
