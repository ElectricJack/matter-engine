
## autoremesher integration [DONE]

Vendored MIT-licensed headless subset of huxingyi/autoremesher into
`Libraries/autoremesher_core/`. Added `MeshIndexed`/`MeshTransform` boundary
in MatterSurfaceLib and `mesh_retopo` module alongside the QEM simplifier.
Retopo is per-part opt-in via `static retopo = { enabled: true, ... }` on the
DSL part class definition, discovered by ScriptHost::eval_retopo_settings.
Wiring landed in the viewer's LocalProvider (RecordingBaker decorator maps
resolved_hash → RetopoSettings, threaded into `part_flatten::flatten_part`
via `FlattenTargets.retopo`). TBB warm-up runs once per viewer process before
any bake to sidestep the WSL2 first-call segfault. First opt-in: Meadow's
Tree. Design: `docs/superpowers/specs/2026-07-07-autoremesher-integration-design.md`.
Plan: `docs/superpowers/plans/2026-07-07-autoremesher-integration.md`.

Task 15 report note: real Meadow-scale Tree geometry (~100k tris of dense
non-manifold voxel bark) triggers a hard geogram abort() inside
autoremesher_core that the C++ try/catch in the MSL retopo() wrapper cannot
recover from. Task 14's end-to-end integration test on a spherified-cube
fixture passes. The wiring is proven; the "subprocess isolation for crash
safety" follow-up below is the correct fix for real Meadow-scale opt-ins.

Follow-ups (out of scope for this landing): mesher-native indexed emit
(remove Tri-boundary conversion at MSL entrance), per-cluster retopo with
boundary preservation, subprocess isolation for crash safety (mandatory
before real Meadow-scale schemas can safely opt in).

---

## Fix HiZ occlusion false-positives (default is OFF because of this)

**Backlog — surfaced 2026-07-05 during freehand Windows testing.**

The Task 10 HiZ occlusion test uses the **previous frame's** max-depth pyramid.
At freehand camera angles / after quick camera motion, the stale pyramid
occludes geometry that is genuinely visible this frame — terrain tiles and
tree segments disappear rectangular-tile-shaped. Task 10 measured pixel-clean
diffs only at 5 fixed poses; freehand camera hits angles those didn't. HUD
`hiz culled` counter climbs into the thousands during the artifact.

Default flipped OFF as a mitigation. Full infrastructure ships: cull.comp
block, pyramid build (Task 9), HUD checkbox, FIFO `hiz on|off`, MATTER_HIZ
env, stats readback, 4-phase unit test. Opt in via any of those toggles.

Correct fix options (pick one):
- **Same-frame conservative depth**: emit a coarse per-cluster near-depth into
  an SSBO in the vertex phase of a prepass, reduce to a pyramid before the
  main cull dispatch. Costs one extra pass but eliminates staleness.
- **Scissor-refined redraw**: keep the previous-frame HiZ but issue a second
  cull+draw pass that reprocesses screen regions where the camera delta
  exceeds a threshold. Cheap when the camera isn't moving.
- **Reprojection heuristic**: dilate the pyramid by the camera's per-frame
  angular delta before sampling. Simplest, still lossy.

Also worth trying regardless: switch the 4-corner samples in the shader to a
2×2 or 3×3 grid across the clamped rect, so partial-off-screen AABBs aren't
sampled only at the clipped edges.

Cross-refs: `MatterEngine3/viewer/shaders_gpu/cull.comp` (hiz block),
`MatterEngine3/viewer/gpu_culler.cpp` (`build_hiz`, `downsample_pyramid`),
`.superpowers/sdd/task-10-report.md`.

---

## P2: compact GPU cull xforms SSBO — 275 MB at 500k trips plan's ~200 MB gate

**Backlog / follow-up — surfaced from GPU instancing/culling Stage-4 stress sweep (2026-07-05).**

At 500k instances the xforms SSBO grew to 274.7 MB (one part × one cluster ×
kMaxLod (9) × region_cap (500000) × sizeof(mat4) (64) = 288 M bytes). The plan
defined an explicit P2 gate: stop and surface to Jack if the SSBO exceeds
~200 MB at 500k. The gate tripped; Task 11 was committed with the measurement
recorded and 500k was NOT chosen as the shipped ceiling — Meadow and worlds up
to 200k are comfortable (< 110 MB).

Two P2 designs from the design doc worth revisiting:
- **Compact emit buffer sized to the visible-cluster count per frame** (skips
  the per-LOD × region_cap over-allocation entirely).
- **Drop kMaxLod × region_cap slots to region_cap × 1** — the cull shader
  picks LOD per instance, one row of xforms is enough.

Also: `resolve_ms` scales super-linearly (1.9 ms at 200k → 20.9 ms at 500k;
10× for 2.5× instances), and `build_ms` similarly (14.5 → 55 ms). The CPU cull
pipeline has its own scaling problem separate from the SSBO — profile before
committing to a P2 design so the fix targets the right bottleneck.

Cross-refs: `docs/superpowers/plans/2026-07-03-gpu-instancing-culling.md`
(P2 section), `MatterEngine3/docs/perf/stress_sweep.csv`,
`.superpowers/sdd/task-11-report.md`.

---

## Harden bake against OOM (three-part plan)

**Backlog / plan — surfaced from GPU instancing/culling Stage-4 stress fixture
(2026-07-05).** Replaces the earlier "Investigate Tree flatten OOM" placeholder.

### Diagnosis (already done)

`part_flatten.cpp` runs top-down per root and merges every triangle from every
child instance into a single monolithic mesh before clustering:
StressForest50k × Tree.part (~100k tri) ≈ 5 B triangles × 72 B TriEx ≈ 360 GB
peak. Viewer aborts with `std::bad_alloc`; no structured error surfaces.

The pipeline does NOT persist intermediate `.flat.part` artifacts for
non-root parts, and does not decide per-parent whether flatten or instancing
wins. Flatten is an unconditional bake-time optimization designed for
hand-authored composites, applied uniformly to scatter-style scatter content
where it's catastrophically wrong.

### Fix plan (in dependency order)

**#1 — Bad_alloc safety net.** Wrap every `part_flatten` / `bake_source` /
`world_flatten` call site in a `try { … } catch (const std::bad_alloc&)` that
converts to a `BakeError::OutOfMemory{root_hash, phase}` and surfaces cleanly
to the viewer log. Doesn't fix the OOM but turns crash → actionable error.
Small (~50 LOC across a handful of files); ships independently.

**#2 — Per-part flatten decision + persistent intermediate `.flat.part`.**
Change the flatten pipeline from "unconditionally merge at the root" to a
per-part decision based on estimated merged size:

```
for each part P in the graph:
    merged_size(P) = own_tris(P) + Σ over children c: subtree_size(c) × count(c)
    if merged_size(P) ≤ BUDGET:
        flatten P into P.flat.part
    else:
        mark P as instance-boundary; parents reference P.flat.part by hash
```

Every part gets its own `.flat.part` (leaves get their baked mesh, composites
get their flattened mesh — up to the budget). Parents that DO flatten inline
the flat.part of children that stayed within budget; instance-boundary
children stay as instance references. GpuCuller consumes those references
via the existing indirect-draw path — no change required on the runtime side.

Applied to the failing cases:
- Meadow: Tree.flat.part small (unchanged), Meadow.flat.part small (unchanged).
- StressForest{N}: Tree.flat.part small, StressForest.merged_size ≫ budget →
  StressForest becomes an instance boundary; 50k instance refs go directly
  to the GpuCuller. Crash prevented.
- 25k Trees + 25k Rocks: same — total merged size dominates, not any single
  hash's dominance. Budget triggers, StressForest stays instance-boundary.

Key design decisions to nail down BEFORE implementing:
- Budget default (proposed: 512 MB of TriEx as an initial ceiling; expose as
  a `FlattenTargets` field so schemas / build scripts can override).
- Estimation source: precomputed `tri_count` from each child's cache metadata,
  no per-child load required.
- Cache invalidation: `.flat.part` cache key must depend on the child's own
  hash AND its flatten decision (leaf vs instance-boundary), so that flipping
  a child from "flattens" to "instance-boundary" invalidates parents.
- Backward compat: existing single-`.flat.part`-at-root worlds keep working;
  new intermediate artifacts are additive.

**#3 — Streaming flatten (peak-memory refactor).**
Even after #2's decision rule keeps most content out of pathological flatten,
the flatten cases themselves can be more memory-efficient. Two variants:

1. *Cluster-streaming*: pre-compute the flat merged AABB in a cheap first
   pass (add up child AABB × transform, no per-tri work), pick the cluster
   grid, then a second pass emits each child's transformed triangles
   directly into cluster buckets. Peak memory = per-bucket state + largest
   single child's per-triangle burst.
2. *Child-streaming*: after grid is picked, process one child at a time — 
   expand its tris, distribute to clusters, decimate that portion, discard.
   Peak = single-child expansion + all-cluster-decimation state.

Peak-memory reduction ~20-30× for meadow-scale content; makes the budget
in #2 rarer to trip and lets us raise its ceiling.

### Order and cost estimate

| # | Item | Risk | LOC est. | Ships value |
|---|------|------|----------|-------------|
| 1 | bad_alloc catch | low | ~80 | debuggable errors immediately |
| 2 | per-part flatten decision + intermediate `.flat.part` | medium (cache invalidation) | ~400 | fixes scatter-style OOMs |
| 3 | streaming flatten | medium (refactor) | ~600 | raises safe ceiling ~30× |

Cross-refs: `.superpowers/sdd/task-11-report.md`,
`MatterEngine3/src/part_flatten.cpp` (Gatherer::gather top-down expansion),
`MatterEngine3/examples/world_demo/schemas/StressForest*.js`.

---

## SurfaceLib Project [DONE]

	This project should define and demonstrate a basic marching cubes algorithm that can convert a series of particles of some fixed size into a mesh surface.

	# Process
	* Create the subdirectory [DONE]
	* Initialize the git repo in the subdirectory [DONE]
	* Get marching cubes surface generation working in a robust manner [DONE]



	## ObjectAlocatorLib Project [DONE]

	This project should define a simple object allocator that can grow in size with pages of objects as needed.

	# Process
	* Work entirely inside the ObjectAlocatorLib directory [DONE]
	* Add a main.c in the subdirectory root that will be for [DONE]
	* Add a makefile [DONE]
	* Implement the OBJECT_ALLOCATOR.md design following test driven development by writing tests first in main.c and stubbing out the interfaces so the project builds but the tests fail. Then one by one implement the object allocator based on the design document so the tests pass. [DONE]

## OpenParticleSurfaceLib

	Implement test project for dynamically building a mesh for thousands of particles, the shared code should live in src/include

	Requirements:
	- Use SurfaceLib to generate isosurface meshes
	- Use ObjectAllocatorLib as a memory manager for particles 
	- Must have extremely high performance

	API Functionality
	- Able to request a chunk of new particles at a world space location [DONE]
	- Position any particles we already know about [DONE]
	- Notify system to update (This sets the dirty flag on bounds within the system) [DONE]


	# Process:
	* Utilize create_project.sh to: [DONE]
		* Create the subdirectory structure [DONE]
		* Initialize the git repo in the subdirectory [DONE]
	* Create an install script that sets up syminks to ObjectAllocatorLib and SurfaceLib [DONE]
	* Run the install script
	* 

## SpatialQueryLib 

	Implement a series of reusable spatial query data structures

	# Initial Setup

	* Utilize create_project.sh to:
		* Create the subdirectory structure
		* Initialize the git repo in the subdirectory
		* Add some initial files to the repo

	* Create hard links to source/include files from ObjectAlocatorLib into our include/src directory, this will be useful in our implementation
	* Create a main.c that executes test cases similar to the ObjectAlocatorLib project
	* Verify everything builds and runs including basic initial tests
	* Commit your changes at this point
	* Then implement the generic SpatialHash framework using Test Driven Development, when you implement failing tests first, commit your changes, then work on your implementation incrementally getting the tests to pass, and committing your changes with each step of progress

	# SpatialHash

	Implement a generica spatial hashing framework.
	Please use OpenParticleSurfaceLib's spatial hash implementation as reference, as this will be the first usecase we would want to replace with this shared code.

	```c
	// Pre-build spatial hash of objects
	typedef struct {
	    void** buckets;     // Array of particle lists
	    int*   bucketSizes;       // Number of particles per bucket
	    int    bucketSize;
	    float  cellSize;         // Size of each hash cell
	} SpatialHash;
	```

## GPURayTraceExample

	Implement a pixel-shader based raytracing example. Start with a Raylib based example window, you can copy BasicWindowApp as a starting point.

	# Setup the project folder
	Create the project folder and initial files and git repo by running the create_project.sh script

	# Build a test scene with some boxes and spheres converted to triangular meshes for ray tracing
	The meshes will be used for unified injestion into the BVH tree
	Assign the meshes different material ids, these material ids will be used in the


	# Build and upload an acceleration structure
	* Construct a BVH tree on the CPU
		* Implement the BVH tree inside the SpatialQueryLib project
		* Add functionality to flatten the tree into node and index buffer arrays

	* Flatten it into two big arrays in GPU memory:
		Node buffer: each node packs an AABB (minXYZ, maxXYZ), a “leaf flag” and either child offsets or triangle‐range indices.
		Index buffer: for leaf nodes, a list of triangle indices.

	* Upload both as SSBOs (GLSL) or UAVs (HLSL/VK).


	# Shoot primary rays per pixel
	In your fullscreen pass (pixel shader) do:

	```glsl
	// GLSL-style pseudocode
	vec3 ro = cameraPos;
	vec3 rd = normalize( (pixelUV.x*2-1)*right*aspect + (pixelUV.y*2-1)*up + forward );

	// traverse BVH
	Hit closest = traceBVH(ro, rd);

	// shade
	vec3 color = (closest.hit ? phongShade(closest) : skyColor);
	```


	# BVH traversal (iterative, stack-based)
	```glsl
	Hit traceBVH(vec3 ro, vec3 rd){
	    int stack[64];
	    int sp = 0;
	    stack[sp++] = 0;              // start at root
	    Hit best = {false, +∞};

	    while(sp){
	        int nodeIdx = stack[--sp];
	        Node n = nodes[nodeIdx];

	        // branchless AABB test first
	        if(!intersectAABB(ro, rd, n.min, n.max, best.t)) continue;

	        if(n.isLeaf){
	            for(int i = n.start; i < n.end; ++i){
	                Triangle tri = tris[ triIndices[i] ];
	                float t, u, v;
	                if(intersectTri(ro, rd, tri, t, u, v) && t < best.t){
	                    best = {true, t, tri.normal, tri.material};
	                }
	            }
	        } else {
	            // push children (better: near child first for coherent early-outs)
	            stack[sp++] = n.child[0];
	            stack[sp++] = n.child[1];
	        }
	    }
	    return best;
	}
	```
	Tips for speed:

	* Avoid recursion: use your own fixed‐size stack.
	* Pack tightly: align node data to vec4 boundaries so each fetch is coalesced.
	* Test AABB before triangles to cull whole subtrees cheaply.
	* Front-to-back order: sort child visits by which AABB you hit first so you can early-exit when t < next node’s entry.
	* SoA vs AoS: try organizing node fields in separate arrays (structure-of-arrays) if your GPU has trouble with wide structs.


	# Add reflections (simple path-tracing loop)

	```glsl
	vec3 pathTrace(vec3 ro, vec3 rd){
	    vec3 throughput = vec3(1);
	    vec3 accum = vec3(0);
	    for(int bounce=0; bounce < MAX_BOUNCES; ++bounce){
	        Hit h = traceBVH(ro, rd);
	        if(!h.hit) { accum += throughput * skyColor; break; }
	        // compute shading; here: perfect mirror
	        vec3 N = h.normal;
	        rd = reflect(rd, N);
	        ro = h.pos + N * 1e-4;
	        throughput *= h.material.reflectance;
	        // optional Russian roulette to terminate early
	        if(max(throughput) < 0.05) break;
	    }
	    return accum;
	}
	```


# Particle Dynamics Example

Demonstrate some of the basic particle dynamics we want to support, and narrow in on the datastructures needed to create the simulation.

Define some different particle types and reactions

- Water particles    (Gas/Liquid/Solid)
- Iron particles     (Liquid/Solid)
- Copper particles   (Liquid/Solid)
- Gold particles     (Liquid/Solid)
- Oxygen particles   (Gas/Liquid)
- Hydrogen particles (Gax/Liquid)
- Rock particles     (Liquid/Solid)
- Carbon particles   (Liquid/Solid)


**Particle Sandbox Simulation Design Document**

This document outlines the essential data structures, material behaviors, simulation systems (thermal, electrical, and chemical), and bonding rules for a dynamic particle-based sandbox world.

---

## 1. Core Particle Structure

Each particle has the following attributes:

* **materialId**: enum of MaterialType
* **position**: (x, y) coordinates
* **velocity**: (vx, vy) vector
* **temperature**: current temperature (°C)
* **charge**: electrical charge (C)

```cpp
struct Vector2 { double x, y; };

struct Particle {
    MaterialType material;
    Vector3 position;
    Vector3 velocity;
    double  temperature;
    double  charge;
};
```

---

## 2. Material Properties Table

| Material     | Density (kg/m³) | Heat Cap. (J/kg·K) | Heat Trans. (W/m·K) | Melt E (J/kg) | Vapor E (J/kg) | Emissivity | Elec. Cond. (S/m) | Permittivity | Spark Thresh. (V/m) |
| ------------ | --------------- | ------------------ | ------------------- | ------------- | -------------- | ---------- | ----------------- | ------------ | ------------------- |
| **Water**    | 1 000           | 4 184              | 0.6                 | 334 000       | 2 260 000      | 0.95       | 5×10⁻⁶            | 80           | 3×10⁶               |
| **Oxygen**   | 1.43            | 918                | 0.026               | 139 000       | 213 000        | 0.20       | 1×10⁻¹⁸           | 1.0005       | 3×10⁶               |
| **Hydrogen** | 0.09            | 14 300             | 0.18                | 60 000        | 455 000        | 0.10       | 1×10⁻¹⁸           | 1.0001       | 3×10⁷               |
| **Carbon**   | 1 800           | 710                | 1.5                 | 113 000       | 360 000        | 0.80       | 1×10⁴             | 10           | 2×10⁶               |
| **Rock**     | 2 700           | 800                | 2.5                 | 250 000       | 1 000 000      | 0.90       | 1×10⁻⁸            | 5            | 3×10⁶               |
| **Wood**     | 600             | 1 700              | 0.12                | 200 000\*     | —              | 0.90       | 1×10⁻⁹            | 4            | 2×10⁶               |
| **Plant**    | 400             | 2 500              | 0.20                | 150 000\*     | —              | 0.90       | 1×10⁻⁸            | 8            | 2×10⁶               |
| **Iron**     | 7 874           | 450                | 80                  | 272 000       | 6 200 000      | 0.30       | 1×10⁷             | —            | —                   |
| **Copper**   | 8 960           | 385                | 400                 | 205 000       | 4 700 000      | 0.05       | 5.9×10⁷           | —            | —                   |
| **Gold**     | 19 320          | 129                | 320                 | 63 700        | 1 630 000      | 0.02       | 4.1×10⁷           | —            | —                   |
| **Oil**      | 800             | 2 000              | 0.15                | 200 000       | 800 000        | 0.95       | 1×10⁻¹⁰           | 3            | 5×10⁶               |
| **Uranium**  | 19 050          | 116                | 27                  | 50 000        | 600 000        | 0.30       | 3×10⁶             | —            | 1×10⁷               |

\*Notes: Melt Energy for wood/plant approximates pyrolysis heat. "—" indicates not applicable.

---

## 3. Thermal Simulation

1. **Conductive Heat Transfer**

```cpp
// For each neighbor pair (i, j):
double Q = (k * area * (T[j] - T[i]) / dx) * dt;
T[i] += Q / (mass[i] * c[i]);
T[j] -= Q / (mass[j] * c[j]);
```

2. **Phase Changes**

```cpp
// After temperature update:
if (T[i] >= meltPoint[i] && state[i] == State::Solid) {
    double needed = latentMelt[i] * mass[i];
    if (heatAvailable >= needed) {
        state[i] = State::Liquid;
        heatAvailable -= needed;
    }
}
```

3. **Radiative Cooling** (optional)

```cpp
// Stefan–Boltzmann cooling
double P = emissivity[i] * sigmaSB * area * pow(T[i], 4);
T[i] -= P * dt / (mass[i] * c[i]);
```

---

## 4. Reaction System

```cpp
#include <unordered_map>
#include <vector>

struct Reaction {
    std::unordered_map<MaterialType, int> reactants;
    std::unordered_map<MaterialType, double> products;
    double activationTemp;
    double energyChange; // J per reaction event
};

std::vector<Reaction> reactions = {
    // Wood + O2 → Carbon + Water
    { {{MaterialType::Wood,1}, {MaterialType::Oxygen,2}}, {{MaterialType::Carbon,1}, {MaterialType::Water,2}}, 300.0, -1.8e7 },
    // Hydrogen + O2 → Water
    { {{MaterialType::Hydrogen,2}, {MaterialType::Oxygen,1}}, {{MaterialType::Water,2}}, 600.0, -2.86e8 },
    // Iron oxidation (rust)
    { {{MaterialType::Iron,1}, {MaterialType::Oxygen,1}}, {{MaterialType::IronOxide,1}}, 50.0, -8e4 }
};

// In each tick:
for (auto& reaction : reactions) {
    if (localTemp >= reaction.activationTemp && hasReactants(reaction.reactants)) {
        consumeParticles(reaction.reactants);
        spawnParticles(reaction.products);
        addHeat(reaction.energyChange);
    }
}
```

---

## 5. Electrical Simulation

1. **Current Flow**

```cpp
// For neighbor pair (i, j):
double I = sigma * area * (V[j] - V[i]) / dx;
charge[i] += I * dt;
charge[j] -= I * dt;
```

2. **Joule Heating**

```cpp
double heat = I * I * R * dt;
T[i] += heat / (mass[i] * c[i]);
T[j] += heat / (mass[j] * c[j]);
```

3. **Dielectric Breakdown**

```cpp
if (abs(V[j] - V[i]) / dx > breakdownField && material[i] == MaterialType::Gas) {
    material[i] = MaterialType::Plasma;
    temperature[i] = highTemp;
}
```

---

## 6. Bonding & Clustering

* **Cohesion**: self-stickiness (0–1)
* **Adhesion matrix**: pairwise stickiness
* **Bond formation**: if stickiness > random() & Δv < threshold → create bond.
* **Bond strength**: strength = stickiness × baseStrength.
* **Bond break**: if force > strength → remove bond.

Use `bondedNeighbors` to build clusters (graphs). Clusters can be treated as rigid bodies or cohesive masses for mining, explosions, and transport.

```
// Example adhesion values:
adhesion = {
  Rock:      { Rock:0.85, Iron:0.80, Copper:0.75, Water:0.10, Oil:0.05, Wood:0.20, … },
  Iron:      { Rock:0.80, Iron:0.80, Copper:0.70, Water:0.05, Oil:0.05, … },
  Copper:    { Rock:0.75, Iron:0.70, Copper:0.75, Oil:0.05, … },
  Gold:      { Rock:0.70, Iron:0.65, Gold:0.70, … },
  Wood:      { Rock:0.20, Iron:0.10, Water:0.30, Oil:0.15, Wood:0.60, Plant:0.50, … },
  Plant:     { Wood:0.50, Water:0.40, Plant:0.50, … },
  Water:     { Water:0.05, Wood:0.30, Plant:0.40, Rock:0.10, … },
  Oil:       { Water:0.05, Wood:0.15, Oil:0.10, Rock:0.05, … },
  Carbon:    { Carbon:0.65, Iron:0.40, … },
  Oxygen:    { /* all zeros */ },
  Hydrogen:  { /* all zeros */ },
  Uranium:   { Rock:0.50, Iron:0.60, Uranium:0.90, … },
}
```


---

## 7. Friction (optional)

Implement static (μs) and dynamic (μk) friction coefficients in your contact resolution:

```cpp
double frictionForce = mu_k * normalForce;
```



*End of C++-updated Design Document*


















# MatterSurfaceLib

We want to combine the raytracing and surface mesh building into a project that can support LOD and rendering of billions of meshed static particles (under the hood)

Definitions:

	Cluster
		- This is a class that manages a group of Cells
		- Clusters have a transform (position + rotation) no scale
		- Everything owned by the cluser is stored in the local coordinate system of the cluster
		- Clusters define the smallest cell size
		- Clusters contain an array of static particles they own, these particles define the matter inside a cluster.

	Cell 
		- This is a storage cube inside the cluster
		- It has integer coordinates for its position
		- Its size an integer that is a power of two of the smallest cell size allowed in the cluster
		- Each cell has a mesh that is constructed from the clusters static particles


	You should be able to add particles to a cluster, which in turn causes the cluster to tell specific cells they need to rebuild, or even create new cells to contain the addded particles.

	You should also be able to remove particles from a cluster







Later:
	- Data Storage interface
	- Disk Storage Provider (implements Data Storage interface)
	- Streaming of data into runtime memory structure from Data provider interface

This project should define a fast mesh simplification algorithm that can maintain mesh boundary conditions for each cell


What should be the interface for having millions of particles, some static and some dynamic that need to update meshes

Also some bodies (group of particles) may be dynamic, but the particle motion internal to the body is static, so the mesh only transforms, but does not update.







# Build ParticleDynamicsLib Project

This project should be built on top of the existing high performance C based physics 
engine ODE (OpenDynamicsEngine) 

We want to define a physics system for the matter particles here that supports both static and dynamic particles.

* Create the project directory
* Create the git repo for the project
* Download ODE into the Libraries folder
* Create the app code for a basic test of ODE based off the BasicWindowApp project, but add a rotating cube that contains some particles bouncing around and interacting

* Carefully define the generic properties of all particles
* Define how the particles should update

- The particles should have some properties like tempurature/energy level
- We should have a series of rules between contacting particle types
	- Do they bind, repel, etc also based on thier current state (low energy vs high energy)
	- How do they transfer energy to neighbor particles



- The particles themselves should define thier material type
	- What are some initial material types we should use? Ideally base these off known elements so we
	  can have some understanding of interactions that might take place
	  	- Water
	  	- Alcohol
	  	- Iron
	  	- Steel
	  	- Aluminum
	  	- Granite
	  	- Concrete
	  	- Glass
	  	- Quartz
	  	- Oxygen
	  	- Hydrogen
	  	- Mercury



Initial Game:


	The game idea should be asteroid mining, make money, upgrade ship, don't die.
		(How simple can I make this? How quickly can this come to market?)

	3D rotations, 2D gameplay world, raytraced everything, HDR, bloom, etc

