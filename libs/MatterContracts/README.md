# MatterContracts 1.0

`matter_contracts` is the owned, neutral recipe/asset boundary described in
[contracts v1](../../docs/component-separation/2026-10-10-matter-component-contracts-v1.md).
It implements data validation and service-boundary checks, using only the C++17
standard library and MathLib's neutral headers. Public interfaces describe
geometry, resolver/cache/host, graphics and terrain services; production
evaluators, backends, scheduling and domain runtimes are supplied separately.
See [service contracts](SERVICES.md) for ownership, callbacks, ABI rules,
fixture IDs and independently selectable build scaffolds.

The local CMake file works standalone and with `add_subdirectory`. Central
registration belongs to the ordered integration work. Nothing in this target
changes the existing native engine or its legacy caches.

## Build and check

From native Windows PowerShell, the local wrapper uses the repository's supported
`Resolve-MatterWindowsToolchain` resolver and pinned MSVC environment:

```powershell
libs/MatterContracts/tests/prepare-test-python.ps1
libs/MatterContracts/build-windows.ps1 -Config RelWithDebInfo -Test -Matrix `
  -TestPython "$PWD/libs/MatterContracts/build/test-python/Scripts/python.exe"
```

From WSL, run that same wrapper with Windows PowerShell and a Windows path:

```bash
/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe \
  -NoProfile -ExecutionPolicy Bypass \
  -File "$(wslpath -w "$PWD/libs/MatterContracts/build-windows.ps1")" \
  -Config RelWithDebInfo -Test -Matrix \
  -TestPython "$(wslpath -w "$PWD/libs/MatterContracts/build/test-python/Scripts/python.exe")"
```

The standalone portability check on Unix uses:

```bash
cmake -S libs/MatterContracts -B /tmp/matter-contracts-build \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/matter-contracts-build -j 2
ctest --test-dir /tmp/matter-contracts-build --output-on-failure -j 1
python3 libs/MatterContracts/tests/schema_tests.py \
  --vectors /tmp/matter-contracts-build/matter_contracts_vectors
```

The schema check uses the test-only Python `jsonschema` package pinned in
`tests/requirements.txt`. It is not a library/runtime dependency. The preparation
script creates an isolated native venv using the supported toolchain resolver;
from WSL invoke it through native PowerShell in the same way as the build wrapper.
With `BUILD_TESTING=ON`, Python and Node are required and CTest runs the schema,
frozen manifest, independent native/C ABI/graphics consumers, legacy MathLib/RNG
compatibility and existing data/service checks. Missing tools/dependencies fail
the gate. `BUILD_TESTING=OFF` consumers do not need Python or Node.
The JS check launches the native
vector executable and checks actual cross-language numeric encodings, SHA-256,
RNG words/unit draws and document digests. Both public-header consumers link only
`matter_contracts`. The native build treats library warnings as errors.

## Owned data and validation

Construct `RecipeDocument` / `AssetDocument` by value, then call `validate` or
`encode`. Publish a successful asset through a const owner, for example
`std::shared_ptr<const AssetDocument>`; builders stay mutable before publication.
The contract carries no pointers or runtime handles. Resource descriptions carry
content identity and typed lengths; the caller supplies the actual resolved
payload to a later evaluator. Validation here cannot verify absent resource bytes.

`decode_recipe`, `decode_asset`, `parse_json`, `encode`, `canonical_json` and
`content_digest` leave their output unchanged on failure. A failed decode never
publishes a partial document. Low-level `to_value` is for inspected, valid data;
use the checked encoders at a public boundary.

Diagnostics have stable codes, severity, source URI/line/column, node identity and
dependency paths. Recipe cycles include a dependency path and source-map location.
Cancellation, stale-result and backend-failure codes are available to services;
this data-only library does not run or cancel evaluations.

Default budgets are 10,000 nodes/prototypes, 100,000 stored instances, 1,000,000
vertices, 3,000,000 indices, dependency/JSON depth 128, 1,000,000 JSON values,
64 MiB encoded document and external resource bytes each, 65,536 bytes per string,
and 64 diagnostics. Callers can set smaller `ValidationLimits`.
All geometry values must be finite float32; general parameters and terrain origins
are finite binary64. Length arithmetic and fixed array sizes are checked.

The structural [JSON schema](schema-v1.json) publishes every wire field. It cannot
express graph cycles, supported capabilities, cross-buffer lengths or CSG transform
restrictions; the C++ semantic validators enforce those. All declared fields are
required in wire objects, including empty arrays and explicit null optional fields.
Unknown structural fields fail. New meanings require a declared version/capability
or a later major schema; v1.0 consumers reject other schema versions.

## Identity and canonical encoding

`SemanticId` is an authored string matching `[A-Za-z0-9][A-Za-z0-9._:/-]{0,255}`.
It never substitutes for `ContentDigest`, which is SHA-256 represented on the wire
as `{"algorithm":"sha256","hex":"<64 lowercase hex digits>"}`. Asset dependencies
pin a semantic ID, content digest and schema version, so editing a dependency
changes the recipe identity while preserving its authored name. Node, material,
texture and resource identities each have their own scope. Mesh and assembly
prototype IDs share a scope. Instance IDs are local to their containing assembly
or the asset's root instance list; parents cannot cross that scope.

`LegacyHash` keeps either `matter.source.v1` or `matter.resolved.v1` explicitly
separate. Adapters translate existing hash meaning; these are never labeled
SHA-256. All uint64 values (seed, legacy hash, resource element count and byte
length) are exactly 16 lowercase hexadecimal digits on the wire. JavaScript
uses BigInt for these values, never Number.

The canonical profile is **`matter-exact-json-v1`**:

1. UTF-8 JSON without a BOM or whitespace; object keys sorted by unsigned UTF-8
   bytes. Unicode normalization is not applied. Reject malformed UTF-8, unpaired
   surrogates, duplicate keys, nonfinite numbers and exhausted budgets.
2. Quote and backslash escape as `\"` and `\\`; U+0000 through U+001F escape as
   lowercase `\u00xx`. Other Unicode scalar values remain literal UTF-8.
3. Encode binary64 numbers by their exact decimal expansion, without exponents,
   trailing fractional zeros or an unnecessary decimal point. Both signed zeros
   encode as `0`. Thus `0.1` becomes
   `0.1000000000000000055511151231257827021181583404541015625`.
   This intentionally trades compactness for a straightforward, identical
   representation in C++ and JS; it is not RFC 8785 / shortest JSON formatting.
4. Arrays retain order, including recipe nodes, outputs, CSG operands, dependencies
   and capability declarations. Forward references are valid if the graph is
   acyclic. Canonicalization never sorts a recipe or commutes CSG inputs.
5. Typed float32 values are rounded to float32 before encoding, then exactly
   widened to binary64. JS authors use `float32()` / `Math.fround()` for positions,
   attributes, matrices and material values. Parameters/origins stay binary64.

`content_digest(document)` validates, removes top-level `metadata` and
`source_map`, removes extension envelopes with `required:false`, then hashes
UTF-8 **`matter-exact-json-v1\n` + canonical JSON**. Optional metadata cannot affect
evaluation. The ordinary encoder retains it for round trips. Provenance, seeds,
dependency digests/versions, material declarations and required extension payloads
remain part of identity. Consumer algorithm/processing versions belong in the
coordinator's cache key or declared semantic extension; this digest alone is not
a complete build-cache policy.

[`examples/contracts.mjs`](examples/contracts.mjs) provides host-independent
canonicalization, float32 normalization, uint64 formatting, SplitMix64 and digest
helpers. The digest helper accepts an injected byte-digest function; its default
uses Web Crypto. It expects a validated, normalized document, not unchecked
JSON text. C++'s strict text parser must be used to detect duplicate JSON keys;
ordinary JS `JSON.parse` loses them before a value-level canonicalizer can inspect
them. See the checked
[recipe](examples/ordered-csg.recipe.json) and
[shared asset](examples/repeated-assembly.asset.json) wire examples.

## Coordinates, placements and materials

Units are metres, seconds and radians; coordinates are right-handed, Y up.
Matrices contain 16 row-major float32 values, act on column vectors, and put
translation at indices 3, 7, 11. `parent * local` applies local first. Quaternions
are x,y,z,w. All meshes declare front-facing CCW triangle winding.

Affine placement accepts invertible mirrored, nonuniform and sheared transforms.
Reject a perspective last row or a singular/near-dependent linear part; the
normalized determinant threshold is `1e-12` relative to the product of column
lengths. `transform_normal` applies the inverse transpose and normalizes using
double intermediates. When `reverses_winding` is true, a consumer that bakes a
placement into vertices must reverse each triangle and flip tangent w. Normals
must use the inverse transpose independently of this winding correction.

For example, `translation(10,20,30) * scale(-2,3,4)` sends `(1,2,3)` to `(8,26,42)`.
The normal `(1,1,0)` becomes proportional to `(-1/2,1/3,0)`. Keeping instances
separate retains their shared prototypes; do not duplicate triangles per placement.
Mesh bounds are in prototype-local coordinates; asset bounds are supplied by the
producer in asset-local coordinates. Validation checks finite ordered bounds and
that prototype bounds contain their vertices; it does not recompute assembled
asset bounds.

Baseline materials are opaque metallic/roughness PBR. Base color factors are
linear RGB in [0,1] with alpha 1, metallic and roughness in [0,1], nonnegative normal
scale and linear nonnegative emissive. Texture descriptions specify dimensions,
`rgba8`, color space, min/mag nearest/linear filters and repeat/clamp/mirror wrapping.
Resource bytes are tightly packed u8x4 texels, row zero at v=0, column zero at u=0;
no implicit vertical flip. RGB sRGB decoding follows the declared color space;
alpha is linear. Packed metallic/roughness maps use roughness G and metallic B.
Normal maps use XYZ RGB mapped from [0,1] to [-1,1]. Data textures require linear
color space. Base color has an explicit UV set; other texture inputs use UV set 0.
Mip generation is consumer-derived data. Advanced materials/transparency require
a separately declared semantic extension and explicit consumer support.

## Core recipe operations, version 1

| Operation | Output / inputs | Parameters / semantics |
| --- | --- | --- |
| `primitive.box` | mesh, none | positive `size:[x,y,z]`, centered box |
| `primitive.sphere` | mesh, none | positive `radius`, integer longitude `segments` 3..65536; latitude subdivisions `max(2,floor(segments/2))`, centered sphere |
| `profile.polygon` | profile, none | `points:[[x,y],...]`, one implicit closed XY ring, at least three points; geometric triangulation checks belong to evaluation |
| `profile.extrude` | mesh, one node/profile | positive `depth`; extrude the XY ring from z=0 to z=depth with caps |
| `solid.csg` | mesh, at least two node/mesh inputs | `operator:union|subtract|intersect`; ordered left fold of closed solids |
| `mesh.input` | mesh, resource inputs | empty parameters; unique named ports `positions`, `indices`, optional `normals`, `tangents`, `uv0`; one whole-mesh material |
| `placement.affine` | mesh or assembly, one matching node input | empty parameters; explicit affine `transform` |
| `assembly.reference` | assembly, one asset/assembly dependency | empty parameters; retain dependency sharing |

Mesh input positions/normals are f32x3, tangents f32x4, UVs f32x2 and indices u32x1.
Buffer scalar bytes are little-endian IEEE float32 / uint32, tightly packed, with
no stride/padding. Element count denotes vectors/texels, not scalar components.
Vertex-attribute counts must match; indices form complete triangles. The resolved
payload validator must check actual indices and finite values before evaluation.
Port labels describe inputs; CSG input array order, not labels, defines its fold.
Other node kinds carry identity transforms. Unknown core parameters fail.

CSG v1 permits translation, rotation and positive uniform scale throughout its
operand dependency graph; mirrored/nonuniform/sheared operands fail with
`unsupported_capability`. Similarity comparison tolerates `1e-5` relative to the
largest squared column length. Plain mesh/assembly placement retains full affine
support. Geometry modifiers use operation name N with declared, supported
`matter.operation.N@operation_version`; unknown required operations never disappear.
Declaring support states a service's capability; this library does not supply the
operation implementation.

## Optional products and capability matrix

| Capability, version 1 | Data contract | Default consumer support |
| --- | --- | --- |
| `matter.recipe.core` | ordered recipe operations above | yes |
| `matter.asset.core` | meshes, shared assemblies and instances | yes |
| `matter.material.pbr` | opaque baseline materials/textures | yes |
| `matter.asset.attachments` | root-instance attachments | explicit opt-in |
| `matter.asset.lod` | prototype reference and absolute error in metres | explicit opt-in |
| `matter.asset.collision` | mesh/convex mesh, box extents or capsule radius/total height | explicit opt-in |
| `matter.asset.animation` | rigs, per-vertex four-joint weights, clips and TRS keys | explicit opt-in |
| `matter.asset.terrain` | region key/origin, prototypes, boundary neighbor/policy/spacing/seam resource | explicit opt-in |

Document declarations and `ValidationOptions.supported_capabilities` must both
include a product's capability. Unsupported consumers get an explicit diagnostic.
Rig parent graphs, joint indices, unit-sum weights, increasing key times and
normalized quaternions are validated. Terrain policies identify an algorithm's
seam contract, not resident sector objects. Unknown optional envelopes are
nonsemantic metadata, may be retained, and must never affect evaluation.

## Fixture evidence

Stable fixture IDs in `tests/contracts_tests.cpp` / `tests/fixtures.h` are
`ordered-csg-v1`, `extruded-prop-v1`, `repeated-assembly-v1`, `typed-products-v1`,
`explicit-mesh-v1`, `assembly-recipe-v1`, `texture-material-v1`, `malformed-v1`,
and `exhaustion-v1`. They exercise data validation/ownership and exact round trips,
not evaluated geometry, rendering or runtime feature extraction. Cancellation
execution belongs to the service-interface fixtures in the next contract task.

[`rng-vectors.json`](tests/rng-vectors.json) pins eight uint64 words and eight
unit draws for seeds 0, 1, 0x0123456789abcdef and UINT64_MAX. Algorithm
`matter.splitmix64.v1` exactly matches the existing native `dsl::Rng`: zero seed
maps to 0x9e3779b97f4a7c15; arithmetic wraps modulo 2^64; unit draws use the top
53 bits divided by 2^53. This is distinct from shared-lib/rng.js's xoshiro128**.
The numeric corpus additionally checks 267 binary64 patterns, including signed
zeros, extrema, subnormals, exponent transitions and fractional values.

## Property tests and fuzzing

The default CTest build includes `matter_contracts_property_json`,
`matter_contracts_property_recipe` and `matter_contracts_property_asset`. Each
has a five-second timeout. They generate small, seeded, in-memory inputs without
reading fixtures from disk. Failures print the area and seed, including in Release
builds; replay a document case with `matter_contracts_properties recipe <seed>`
or `matter_contracts_properties asset <seed>`. The oracles check exact canonical
round trips, binary64 preservation,
unchanged outputs on failure, independent copies of nested data, ordered CSG
identity, metadata exclusion, and rejection of malformed graphs, buffers and
budgets. Run them with `ctest --test-dir <build> -L property --output-on-failure`.

The longer harness is opt-in and is not registered with CTest. A Linux sanitizer
build instruments both the contract implementation and the driver:

```bash
cmake -S libs/MatterContracts -B /tmp/contracts-fuzz -G Ninja \
  -DBUILD_TESTING=ON -DMATTER_CONTRACTS_BUILD_FUZZER=ON \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build /tmp/contracts-fuzz --parallel 2
ctest --test-dir /tmp/contracts-fuzz --output-on-failure -j 1
/tmp/contracts-fuzz/matter_contracts_fuzz 10000 1718975098
```

The standalone arguments are iteration count and unsigned decimal seed. It mixes
valid JSON trees, recipes/assets, truncated documents and arbitrary bytes. Every
input also selects a generated document with structural mutations. Replaying the
same driver seed/count reproduces the sequence; an oracle failure prints the
individual document seed.

For coverage-guided fuzzing on Unix with Clang, enable
`MATTER_CONTRACTS_LIBFUZZER` (this mode adds ASan/UBSan itself):

```bash
cmake -S libs/MatterContracts -B /tmp/contracts-libfuzzer -G Ninja \
  -DBUILD_TESTING=OFF -DMATTER_CONTRACTS_BUILD_FUZZER=ON \
  -DMATTER_CONTRACTS_LIBFUZZER=ON -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/contracts-libfuzzer --parallel 2
/tmp/contracts-libfuzzer/matter_contracts_fuzz -max_total_time=60 \
  -max_len=8192 -seed=1718975098
```

The driver exports `LLVMFuzzerTestOneInput`; byte inputs over 8192 bytes are
ignored, and decoder budgets bound depth, counts, strings and diagnostics.
Production budgets are unchanged. Optional corpus paths and libFuzzer crash
artifacts belong to the caller's fuzz run, outside the deterministic CTest suite.
