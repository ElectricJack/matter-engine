# Matter authoring SDK

Portable ECMAScript modules and TypeScript declarations for authoring contract
v1 recipe documents. The public entry point is `index.mjs`, with declarations in
`index.d.mts`. No install, transpilation, or runtime package dependency is needed.
The package requires ES2022 features, including BigInt, private class fields and
`Object.hasOwn`. `legacyPart()` additionally uses Proxy.

The wire format and semantic identity belong to
[`MatterContracts`](../../libs/MatterContracts/README.md) and
the [frozen v1 contract](../../docs/component-separation/contracts-v1.md).
The SDK constructs data; geometry evaluation, dependency discovery, VM execution,
filesystem/network access, and renderer policy belong to the host.

## A small prop

This example imports only the SDK and returns an immutable recipe. Sizes and
positions are in metres, rotations in radians. Seed inputs are uint64 BigInt or
exactly sixteen lowercase hex digits; Number seeds are rejected.

```js
import { RecipeBuilder, parameters } from './index.mjs';

const p = parameters({
  width: { type: 'number', default: 2, minimum: 0.01 },
  enabled: { type: 'boolean', default: true },
});
const b = new RecipeBuilder({
  source: { id: 'prop.crate', uri: 'author://crate.js', revision: '1' },
  seed: '0123456789abcdef',
  parameters: p,
});
const clay = b.material('clay', { baseColor: [0.75, 0.25, 0.125, 1] });
b.at({ uri: 'author://crate.js', line: 14, column: 1 }, () => {
  b.output(b.box('crate', [p.width, 1, 1], { materials: [clay] }));
});
const document = b.build();
```

Parameters support number, integer, boolean, string and scalar enum declarations,
validated defaults, numeric bounds, and typed overrides. Unknown overrides fail.
Recipe values are copied and recursively frozen. Closures, undefined, nonfinite
numbers, typed arrays, accessors, symbols, sparse arrays, invalid Unicode, and
cyclic values fail rather than enter the document.

## Contract services and explicit randomness

`build()` needs no service or ambient host globals. Canonical serialization,
document hashing, and RNG construction use explicitly injected contract services.
The SDK does not copy the pinned canonical serializer or SplitMix64 algorithm.

A repo host module beside `index.mjs` can adapt the delivered contract helper:

```js
import * as contracts from '../../libs/MatterContracts/examples/contracts.mjs';
import { contractServices, RecipeBuilder } from './index.mjs';

// Supplied by the host; accepts bytes and returns Promise<Uint8Array> with
// exactly 32 SHA-256 digest bytes. No implicit crypto service is selected.
const services = contractServices(contracts, sha256Bytes);
const b = new RecipeBuilder({
  source: { id: 'prop.random', uri: 'author://random.js', revision: '1' },
  seed: 1n, services,
});
const rng = b.rng();
const box = b.box('box', [rng.range(1, 2), 1, 1]);
b.output(box);
const canonical = b.canonical();
const digest = await b.digest();
```

The shared helper uses TextEncoder in its host realm. An embedded host may
instead inject its own service implementation through `ContractServices`,
marshalling VM values into the service's realm. The SDK core imports only its
own modules and requires no TextEncoder, crypto, DOM, Node, QuickJS bindings,
clock, filesystem, network, or ambient entropy.

Each `b.rng()` creates a new stream at the recipe's explicit seed. Keep that
stream when multiple draws should advance together. `SeededRng` exposes uint64
words, unit values, integer/range helpers, frozen snapshots, restore and clone.
Zero-seed remapping and the top-53-bit unit conversion follow
`matter.splitmix64.v1`, including its frozen native known-answer vectors.
`digest()` follows the contract semantic projection: metadata, source maps and
optional extension payloads do not affect semantic identity.

## Operations and composition

| Builder | Contract v1 meaning |
| --- | --- |
| `box(id, size)` | Positive full extents, centred at the origin |
| `sphere(id, radius, segments)` | Positive radius; integer segments in [3, 65536] |
| `polygon(id, points)` | At least three finite XY profile points |
| `extrude(id, profile, depth)` | Positive depth from z=0 to z=depth |
| `csg(id, operator, operands)` | Ordered left fold of mesh operands; union/subtract/intersect |
| `mesh(id, buffers, material)` | Explicit typed, content-addressed buffers and one whole-mesh material |
| `place(id, reference, matrix)` | Mesh/assembly placement with full affine transform |
| `assembly(id, dependencyId)` | Reference to an explicitly pinned asset/assembly dependency |

Matrices are row-major and multiply column vectors. Translation occupies indices
3, 7, 11; `multiply(parent, local)` composes in that order with float32 arithmetic.
Mirroring, nonuniform scaling and shear are accepted in affine placements. Every
transform in a CSG dependency graph must be a rotation/translation with positive
uniform scale. Singular matrices fail. Materials and matrices normalize to
float32; recipe parameters retain binary64 values.

Typed node references prevent profile/mesh/assembly confusion in TypeScript.
`nodeInput()` supports forward references, validated at `build()`. Resource counts
accept BigInt/hex inputs and serialize as uint64 hex. Resource contents are SHA-256
addresses, with declared scalar type, components and byte length; hosts validate
the resolved bytes and index contents. Textures use tightly packed RGBA8 buffers
and explicit colour space. Baseline PBR materials are opaque.

Declare each dependency with `dependency({ id, content, schema_version })` after
host discovery has produced a frozen snapshot. Matching IDs deduplicate only
when digest and schema agree; conflicting pins fail. `repeat()` creates explicit
placements using a caller-supplied transform function. `compose(prefix, recipe)`
scopes all local IDs and references, preserves source locations, and shares pinned
asset dependencies. A failed repeat/composition restores the prior builder data.
Composition flattens already-authored nodes; the host retains the original
transitive source snapshot for cache/provenance accounting.

`node()` permits custom operations only with explicit support for
`matter.operation.<operation>@<operationVersion>`. Required extensions must also
be declared and supported. Unknown optional payloads are preserved. Composition
of custom operations or extension-bearing documents fails with an actionable
capability diagnostic: their embedded IDs need a consumer-specific adapter.

## Selected legacy Part compatibility

`legacyPart(builder, options)` and `LegacyPart` translate this explicit subset:

| Verbs | Behaviour |
| --- | --- |
| `pushMatrix`, `popMatrix`, `translate`, `scale`, `rotateX/Y/Z`, `applyMatrix` | Scoped, postmultiplied affine transform cursor |
| `fill` | PBR ID, or native integer ID mapped through `materialIds` |
| `box`, `sphere` | Legacy centre + half-extents/radius; sphere uses 12 segments |
| `beginVoxels`, `endVoxels`, `smoothing` | Explicit voxel meshing capability described below |
| `union`, `difference`, `intersection` | Postfix operator on the preceding brush in the current voxel session |
| `placeChild` | Synchronous host resolver returning a frozen, pinned `AssetDependency` |

The adapter never installs globals, executes a source module, or discovers live
dependencies. Pass `sourceLocation()` to map each invocation to an explicit
URI/line/column. `finish()` checks balanced matrix/session scopes and returns the
validated recipe. A failed final validation seals further legacy authoring; the
host may repair a missing builder declaration and retry `finish()`.

Voxel spacing and whole-expression smoothing cannot be discarded. Opt-in
`LEGACY_VOXELS` declares the required `matter.legacy.voxel-meshing@1` capability.
`finish()` emits one required envelope whose payload is
`{ sessions: [{ output, spacing_metres, smoothing_metres }, ...] }` in session
order. Nonpositive spacing falls back to native float32 0.1m; negative smoothing
clamps to zero. The final smoothing cursor applies to the entire closed session.
This SDK preserves those settings; a host may advertise the capability only when
its evaluator implements them. Default consumers reject the document explicitly.

Nested/empty voxel sessions and postfix operators outside a session fail.
First-brush subtraction/intersection, child renderer/inline options, lattice,
rig/animation, modifiers, tint, raycast, cylinder, shape/path and every other
legacy verb require the native path or a versioned adapter. The Proxy returned
by `legacyPart()` reports unknown verbs; direct `LegacyPart` callers can use
`invoke(verb, ...args)` for the same diagnostic.

Errors carry a frozen `diagnostic` with code, message, node ID, optional source
location, and dependency path. Codes distinguish malformed input, unsupported
version/capability, missing dependency, cycle and resource limit. Source locations
are supplied by the host rather than inferred from VM-specific stack traces.

## Verification

From the repository root, without installing dependencies:

```sh
cmake -S packages/matter-authoring -B /tmp/matter-authoring-check -DBUILD_TESTING=ON
cmake --build /tmp/matter-authoring-check --parallel 2
ctest --test-dir /tmp/matter-authoring-check --output-on-failure
node --experimental-vm-modules packages/matter-authoring/tests/authoring_tests.mjs /tmp/matter-authoring-check/matter_authoring_contract_check
tsc -p packages/matter-authoring/tests/tsconfig.json
node packages/matter-authoring/tests/browser_tests.mjs
```

The CMake harness links the actual `matter_contracts` library and checks native
decode/validation/canonical re-encoding, including extension capability rejection.
It registers Chrome/Chromium conformance when a browser is found. For a standalone
browser run, set `MATTER_AUTHORING_BROWSER` to the executable path if needed.
Browser fixtures are generated in memory, with external DNS disabled. The portable
VM harness disables Node/DOM/crypto/TextEncoder/clock globals and Math.random,
then compares full canonical output with independent expected fixtures. It is a
portable host conformance test; installing a QuickJS execution adapter remains
host integration work.

`SDK-PROP-v1`, `SDK-CSG-v1` and `SDK-REPEATED-v1` compare exact recipes. The CSG
expectation is the existing frozen contract fixture; the other two are literal
expected documents. `SDK-RNG-v1` covers all 32 native words and 32 unit values,
plus stream snapshot/restore/clone. Additional fixtures cover typed parameters,
affine restrictions, composition, buffers/textures, legacy semantics, diagnostics,
deep ownership, bounded DAG paths, service digests, isolated host loading and the
native wire boundary. The type check uses only ES2022 library declarations.

Default budgets match the v1 node/depth/value/string/resource/mesh ceilings;
dependency declarations additionally have a 1024-entry authoring limit. Hosts may
lower these through `limits`. The canonical service enforces exact wire-byte
budgets. No central build manifest, existing ScriptHost/DslState, world module,
or application entry point is modified by this package.
