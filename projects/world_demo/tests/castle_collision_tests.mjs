import assert from "node:assert/strict";
import { castleCollisionEntities } from "../shared-lib/castle_collision.js";

const close = (a, b) => Math.abs(a - b) < 1e-7;
function contains(entity, point) {
  const { translation: p, rotation: q } = entity.components.LocalTransform;
  const h = entity.components.BoxCollider.halfExtents;
  const c = 1 - 2 * q[1] * q[1],
    s = 2 * q[1] * q[3];
  const x = point[0] - p[0],
    y = point[1] - p[1],
    z = point[2] - p[2];
  return (
    Math.abs(c * x - s * z) <= h[0] + 1e-7 &&
    Math.abs(y) <= h[1] + 1e-7 &&
    Math.abs(s * x + c * z) <= h[2] + 1e-7
  );
}
const blocked = (entities, p) => entities.some((e) => contains(e, p));
const manifest = (extras = {}) => ({
  levels: [
    { id: "g", baseY: 0 },
    { id: "u", baseY: 4 },
  ],
  rooms: [],
  floors: [],
  walls: [],
  wallModules: [],
  curves: [],
  stairs: [],
  ...extras,
});
const cells = (x, z, width, depth) =>
  Array.from({ length: width * depth }, (_, i) => [
    x + (i % width),
    z + Math.floor(i / width),
  ]);
const floor = (id, levelId, elevation, overrides = {}) => ({
  id,
  levelId,
  elevation,
  thickness: 0.25,
  boundary: { kind: "cells", cells: cells(0, 0, 8, 8) },
  holes: [],
  ...overrides,
});

// A two-module opening must remain one continuous aperture, not repeat its
// global extent from each module's origin. Its jamb and head remain solid.
const aperture = {
  apertureId: "door-a",
  kind: "arch",
  segmentFrom: [0, 0],
  segmentTo: [4, 0],
  globalStart: 1,
  globalEnd: 3,
  bottom: 0,
  top: 2.4,
};
const doors = manifest({
  wallModules: [0, 2].map((x) => ({
    id: `module-${x}`,
    levelId: "g",
    kind: "arch",
    from: [x, 0],
    to: [x + 2, 0],
    section: { thickness: 0.6, height: 4 },
    apertures: [aperture],
  })),
});
const doorBefore = JSON.stringify(doors);
const doorBodies = castleCollisionEntities(doors);
assert.equal(blocked(doorBodies, [1.25, 1.1, 0]), false);
assert.equal(blocked(doorBodies, [2.75, 1.1, 0]), false);
assert.equal(blocked(doorBodies, [0.75, 1.1, 0]), true);
assert.equal(blocked(doorBodies, [3.25, 1.1, 0]), true);
assert.equal(blocked(doorBodies, [2, 3, 0]), true);
assert.equal(JSON.stringify(doors), doorBefore, "compiler is pure");
assert(
  doorBodies.diagnostics.some((d) => d.code === "ARCH_CLEARANCE_ENVELOPE"),
);
assert.equal(Object.keys(doorBodies).includes("diagnostics"), false);

// Axis-aligned walls along Z and reversed authored endpoints use canonical
// segmentFrom/global spans. Open edges never create full-height obstacles.
const zWall = manifest({
  walls: [
    {
      id: "z-wall",
      levelId: "g",
      kind: "door",
      from: [0, 4],
      to: [0, 0],
      section: { thickness: 0.6, height: 4 },
      openings: [
        { ...aperture, kind: "door", segmentFrom: [0, 0], segmentTo: [0, 4] },
      ],
    },
    { id: "open", kind: "open" },
  ],
});
const zBodies = castleCollisionEntities(zWall);
assert(!blocked(zBodies, [0, 1, 2]));
assert(blocked(zBodies, [0, 1, 0.5]));

// Only void-edge openings receive balcony barriers. Internal open passages and
// exterior doors stay traversable even with a stale rail-required flag.
const balcony = manifest({
  walls: [0, 2, 4].flatMap((z) =>
    [0, 1].map((x) => ({
      id: `rail-edge-${x}-${z}`,
      levelId: "u",
      kind: "open",
      from: [x, z],
      to: [x + 1, z],
    })),
  ),
  openBoundaries: ["void-edge", "room-open", "exterior-entry"].map(
    (kind, i) => ({
      id: `boundary-${i}`,
      levelId: "u",
      kind,
      hostEdgeIds: [`rail-edge-0-${i * 2}`, `rail-edge-1-${i * 2}`],
      rail: { required: true, profile: "castle.guardrail" },
    }),
  ),
});
const rails = castleCollisionEntities(balcony);
assert.equal(rails.length, 1, "adjacent rail edges merge into one body");
assert(blocked(rails, [1, 4.8, 0]), "balcony void edge stops the body");
assert(
  !blocked(rails, [1, 5.2, 0]),
  "railing does not become a full-height wall",
);
assert(!blocked(rails, [1, 4.8, 2]), "internal open passage stays clear");
assert(!blocked(rails, [1, 4.8, 4]), "exterior entry stays clear");
assert(close(rails[0].components.BoxCollider.halfExtents[1] * 2, 1.1));
assert(close(rails[0].components.BoxCollider.halfExtents[2] * 2, 0.12));
const customRail = structuredClone(balcony);
customRail.openBoundaries[0].rail.profile = { height: 1.4, thickness: 0.2 };
const customRails = castleCollisionEntities(customRail, {
  offset: [10, 2, 30],
});
assert(blocked(customRails, [11, 7.3, 30]));
assert(close(customRails[0].components.BoxCollider.halfExtents[2] * 2, 0.2));

// U stairs: lower deck, both flights, intermediate landing and the replacement
// upper landing. The old floor.holes bounding rectangle must NOT eat the deck
// adjacent to the upper landing (x2..4,z0..2).
const regions = [
  { x: 2, z: 2, width: 2, depth: 3 },
  { x: 4, z: 2, width: 2, depth: 3 },
  { x: 2, z: 5, width: 4, depth: 2 },
  { x: 4, z: 0, width: 2, depth: 2 },
];
const stair = {
  id: "u-stair",
  width: 2,
  tread: 0.3,
  riser: 0.2,
  flights: [
    {
      id: "flight-low",
      direction: "N",
      footprint: regions[0],
      stepCount: 10,
      fromY: 0,
      toY: 2,
    },
    {
      id: "flight-high",
      direction: "S",
      footprint: regions[1],
      stepCount: 10,
      fromY: 2,
      toY: 4,
    },
  ],
  landings: [
    {
      id: "landing-low",
      kind: "lower",
      bounds: { x: 2, z: 0, width: 2, depth: 2 },
      elevation: 0,
    },
    {
      id: "landing-mid",
      kind: "intermediate",
      bounds: regions[2],
      elevation: 2,
    },
    { id: "landing-high", kind: "upper", bounds: regions[3], elevation: 4 },
  ],
  holes: regions.map((r, i) => ({
    id: `h${i}`,
    floorId: "upper",
    footprint: r,
  })),
  voids: [{ id: "upper-void", kind: "floor", regions }],
};
const stairPlan = manifest({
  stairs: [stair],
  floors: [
    floor("ground", "g", 0, {
      holes: [
        { kind: "ceiling", footprint: { x: 2, z: 0, width: 4, depth: 7 } },
      ],
    }),
    floor("upper", "u", 4, {
      holes: [
        {
          kind: "floor",
          voidId: "upper-void",
          footprint: { x: 2, z: 0, width: 4, depth: 7 },
        },
      ],
    }),
    floor("void", "u", 4, {
      openToBelow: true,
      boundary: { kind: "cells", cells: cells(20, 0, 2, 2) },
    }),
  ],
});
const stairBodies = castleCollisionEntities(stairPlan);
assert(
  blocked(stairBodies, [3, -0.1, 3]),
  "ceiling hole must preserve ground deck",
);
assert(!blocked(stairBodies, [3, 3.9, 3]), "upper slab is absent above flight");
assert(blocked(stairBodies, [3, 3.9, 1]), "deck beside landing remains");
assert(
  blocked(stairBodies, [5, 3.9, 1]),
  "upper landing replaces removed floor",
);
assert(blocked(stairBodies, [4, 1.9, 6]), "middle landing top is 2m");
assert(!blocked(stairBodies, [21, 3.9, 1]), "air room creates no floor");
const newHoles = structuredClone(stairPlan);
newHoles.floors[1].holes = regions.map((r, i) => ({
  id: `floor-hole-${i}`,
  kind: "stair",
  footprint: r,
  regions: [r],
  replacementLandingId: i === 3 ? "landing-high" : null,
}));
const newBodies = castleCollisionEntities(newHoles);
assert(
  blocked(newBodies, [3, 3.9, 1]),
  "new compiler stair holes preserve adjacent deck",
);
assert(!blocked(newBodies, [3, 3.9, 3]));
const doubleHeight = manifest({
  floors: [
    floor("double-height", "u", 4, {
      holes: [
        {
          kind: "double-height",
          regions: [{ x: 2, z: 2, width: 4, depth: 4 }],
        },
      ],
    }),
  ],
});
assert(!blocked(castleCollisionEntities(doubleHeight), [3, 3.9, 3]));
assert(blocked(castleCollisionEntities(doubleHeight), [1, 3.9, 1]));
for (const [id, fromY, startZ, sign] of [
  ["flight-low", 0, 2, 1],
  ["flight-high", 2, 5, -1],
]) {
  const treads = stairBodies.filter((e) => e.name.includes(`${id}:tread:`));
  assert.equal(treads.length, 10);
  for (let i = 0; i < treads.length; ++i) {
    const e = treads[i],
      t = e.components.LocalTransform.translation;
    const h = e.components.BoxCollider.halfExtents;
    assert(
      close(t[1] + h[1], fromY + (i + 1) * 0.2),
      "every tread top matches exact rise",
    );
    assert(
      close(t[2], startZ + sign * (i + 0.5) * 0.3),
      "treads follow actual flight direction",
    );
    assert(close(h[2] * 2, 0.3));
  }
}

// Circle floor with an exact slab opening; curved walls have real angular door
// gaps even across the 360/0 seam. Rotation is tested by actual point containment.
const curved = manifest({
  floors: [
    floor("circle", "g", 0, {
      boundary: { kind: "circle", center: [0, 0], radius: 4 },
      holes: [
        { kind: "floor", footprint: { x: -1, z: -1, width: 2, depth: 2 } },
      ],
      extensions: [
        {
          kind: "polygon-rect",
          bounds: { minX: 3.8, maxX: 4.6, minZ: -0.8, maxZ: 0.8 },
        },
      ],
    }),
  ],
  curves: [
    {
      id: "ring",
      levelId: "g",
      kind: "ring",
      center: [0, 0],
      radius: 4,
      section: { thickness: 0.6, height: 4 },
      apertures: [
        {
          id: "arc-door",
          kind: "door",
          startAngle: -12,
          endAngle: 12,
          bottom: 0,
          height: 2.4,
        },
      ],
    },
  ],
});
const curveBodies = castleCollisionEntities(curved);
assert(!blocked(curveBodies, [0, -0.1, 0]));
assert(blocked(curveBodies, [2, -0.1, 0]));
assert(
  blocked(curveBodies, [4.5, -0.1, 0]),
  "radial floor patch supports throat",
);
assert(!blocked(curveBodies, [4, 1, 0]), "curve doorway really open");
assert(blocked(curveBodies, [4, 3, 0]), "curve lintel retained");
assert(blocked(curveBodies, [-4, 1, 0]), "opposite curved wall remains");
const angle = (22 * Math.PI) / 180;
assert(
  blocked(curveBodies, [4 * Math.cos(angle), 1, 4 * Math.sin(angle)]),
  "rotated chord colliders cover wall",
);
assert(
  curveBodies.diagnostics.some(
    (d) => d.code === "CURVED_WALL_BOX_APPROXIMATION",
  ),
);
const quarter = manifest({
  curves: [
    {
      id: "quarter",
      levelId: "g",
      kind: "quarter",
      center: [0, 0],
      radius: 4,
      clockwise: true,
      endpoints: [{ position: [4, 0] }, { position: [0, -4] }],
      section: { thickness: 0.6, height: 4 },
      apertures: [],
    },
  ],
});
const quarterBodies = castleCollisionEntities(quarter);
assert(blocked(quarterBodies, [Math.sqrt(8), 1, -Math.sqrt(8)]));
assert(!blocked(quarterBodies, [Math.sqrt(8), 1, Math.sqrt(8)]));

const moved = castleCollisionEntities(curved, {
  prefix: "moved",
  offset: [100, 8, -20],
});
assert.equal(moved.length, curveBodies.length);
assert(!blocked(moved, [104, 9, -20]));
assert(blocked(moved, [96, 9, -20]));
assert(blocked(moved, [102, 7.9, -20]));
for (const e of [...doorBodies, ...stairBodies, ...curveBodies, ...moved]) {
  assert.equal(e.components.RigidBody.type, "static");
  const t = e.components.LocalTransform;
  assert([...t.translation, ...t.rotation].every(Number.isFinite));
  assert(
    e.components.BoxCollider.halfExtents.every(
      (v) => Number.isFinite(v) && v > 0,
    ),
  );
  assert(
    close(
      t.rotation.reduce((sum, v) => sum + v * v, 0),
      1,
    ),
    "rotation is unit quaternion",
  );
}
assert.equal(new Set(stairBodies.map((e) => e.id)).size, stairBodies.length);
assert.deepEqual(
  castleCollisionEntities(curved),
  curveBodies,
  "repeated compilation is deterministic",
);
assert.throws(
  () => castleCollisionEntities(curved, { offset: [0, NaN, 0] }),
  /finite/,
);
assert.throws(
  () =>
    castleCollisionEntities(
      manifest({
        wallModules: [
          {
            id: "broken-door",
            levelId: "g",
            kind: "door",
            from: [0, 0],
            to: [4, 0],
            section: { thickness: 0.6, height: 4 },
          },
        ],
      }),
    ),
  /aperture/,
);
const badFlight = structuredClone(stairPlan);
badFlight.stairs[0].flights[0].toY = 2.4;
assert.throws(() => castleCollisionEntities(badFlight), /destination/);
console.log(
  `castle_collision_tests: PASS (${stairBodies.length} U-stair bodies; ${curveBodies.length} curved fixture bodies)`,
);
