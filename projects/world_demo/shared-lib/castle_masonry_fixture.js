// Castle masonry acceptance fixture.
//
// A single-level floor plan authored purely as data for `compilePlan()`
// (projects/world_demo/shared-lib/castle_plan.js). It exists to exercise, in
// one manifest, every wall-masonry feature the bake pipeline and its Node
// tests need coverage for:
//
//  1. Straight wall-module lengths: the free-standing "bailey-wall" stub
//     (room `bailey-wall`) is an isolated 8x1 rectangle with no overrides, so
//     its long sides merge into a single 8 m wallModule; room `b`'s south
//     wall and room `d`'s north wall are clean, unbroken 4 m runs; the
//     various room side walls (`a`/`c` west, `d` east, ...) give 2 m runs;
//     and the bailey-wall's short ends plus the two 1 m stubs flanking
//     `a`'s window give 1 m runs.
//  2. Windows on exterior walls: `a-south-window` (on room `a`'s south wall)
//     and `b-east-window` (on room `b`'s east wall).
//  3. Doors and an arch: `front-door` is the exterior entry portal (hall to
//     outside); `hall-c-door` and `c-d-door` are interior doors; `b-d-arch`
//     is an interior arch between rooms `b` and `d`.
//  4. Junction kinds: a hall (x:[0,8], z:[0,6]) sits above four rooms
//     a:[0,4]x[-4,-2], b:[4,8]x[-4,-2], c:[0,4]x[-2,0], d:[4,8]x[-2,0].
//     (4,-2) is a cross (all four rooms meet there); (4,0), (0,-2), (8,-2)
//     and (4,-4) are T junctions; (0,-4) and (8,-4) are L corners. The
//     `a-c-open` override punches a gap in the middle of the a/c partition
//     (also doubling as a's connection into the room graph), leaving its two
//     cut ends, (1,-2) and (3,-2), as `end` junctions while leaving the
//     (4,-2) cross intact.
//  5. A quarter-curve apse with a tangent join on both ends: the hall's
//     north-east corner cells are removed (a 2x2 notch, {(6,4),(7,4),(6,5),
//     (7,5)}), the two notch edges are overridden `open` so no straight wall
//     crosses the rounded corner, and curve `hall-apse` fills the corner
//     with a quarter arc from E (8,4) to N (6,6). The hall's east wall
//     (x=8, z<4) and north wall (z=6, x<6) continue away from the arc at
//     each endpoint and provide the tangent wall sockets.
//  6. A ring tower with a radial throat: a circular room `tower` (center
//     [-3,3], radius 3) is tangent to the hall's west wall at (0,3). Curve
//     `tower-ring` carries a radial throat aperture (`tower-throat`) that
//     bridges to the hall through a host arch (`tower-host`) cut into the
//     hall's west wall, plus three window apertures at roughly 90/180/270
//     degrees around the ring.
//
// Every wall/curve section in this fixture uses the same 0.6 m limestone
// ashlar section so the curve<->straight-wall tangent sockets and the
// radial-throat host aperture stay section-compatible with the walls they
// join, per castle_plan.js's `sectionCompatible` check.
//
// No imports: this module is loaded by both plain Node (the masonry Node
// tests) and the engine's QuickJS host.

function rectCells(x0, z0, width, depth) {
  const cells = [];
  for (let dz = 0; dz < depth; ++dz)
    for (let dx = 0; dx < width; ++dx)
      cells.push([x0 + dx, z0 + dz]);
  return cells;
}

// Hall: 8x6 grid minus the 2x2 north-east notch reserved for the apse curve.
const HALL_NOTCH = new Set(['6,4', '7,4', '6,5', '7,5']);
const HALL_CELLS = rectCells(0, 0, 8, 6).filter(([x, z]) => !HALL_NOTCH.has(`${x},${z}`));

export const CASTLE_MASONRY_FIXTURE_PLAN = {
  schema: 'matter.castle-plan/v1',
  id: 'castle-masonry-fixture',
  seed: 11,
  entryRoomId: 'hall',
  style: {
    wallThickness: 0.6,
    wallMaterial: 'limestone',
    bond: 'ashlar',
  },
  levels: [
    {
      id: 'ground',
      baseY: 0,
      height: 4,
      rooms: [
        { id: 'hall', use: 'hall', cells: HALL_CELLS },
        { id: 'a', use: 'guardroom', rect: { x: 0, z: -4, width: 4, depth: 2 } },
        { id: 'b', use: 'guardroom', rect: { x: 4, z: -4, width: 4, depth: 2 } },
        { id: 'c', use: 'storeroom', rect: { x: 0, z: -2, width: 4, depth: 2 } },
        { id: 'd', use: 'storeroom', rect: { x: 4, z: -2, width: 4, depth: 2 } },
        { id: 'tower', use: 'guardroom', boundary: { kind: 'circle', center: [-3, 3], radius: 3 } },
        {
          id: 'bailey-wall', use: 'curtain-wall', required: false,
          rect: { x: 0, z: 20, width: 8, depth: 1 },
        },
      ],
      edgeOverrides: [
        // Exterior entry: hall's north wall, well clear of both the west
        // corner (0,6) and the curve tangent socket at (6,6).
        {
          id: 'front-door', from: [2, 6], to: [4, 6], kind: 'door',
          connects: ['hall', 'outside'],
          opening: { width: 1.3, height: 2.2, bottom: 0 },
        },
        // Interior door: hall down into the store cluster via room c.
        {
          id: 'hall-c-door', from: [1, 0], to: [3, 0], kind: 'door',
          connects: ['hall', 'c'],
          opening: { width: 1.3, height: 2.2, bottom: 0 },
        },
        // Interior door: c to d, sitting between the (4,-2) cross and the
        // (4,0) T junction with >=0.3m clearance from both.
        {
          id: 'c-d-door', from: [4, -2], to: [4, 0], kind: 'door',
          connects: ['c', 'd'],
          opening: { width: 1.2, height: 2.2, bottom: 0 },
        },
        // Interior arch: b to d, well clear of the (4,-2) cross and (8,-2) T.
        {
          id: 'b-d-arch', from: [5, -2], to: [7, -2], kind: 'arch',
          connects: ['b', 'd'],
          opening: { width: 1.8, height: 2.7, bottom: 0 },
        },
        // Open gap in the middle of the a/c partition: gives room a a route
        // into the graph via c, and leaves (1,-2)/(3,-2) as `end` junctions
        // while the (4,-2) cross at the far end stays intact.
        {
          id: 'a-c-open', from: [1, -2], to: [3, -2], kind: 'open',
          connects: ['a', 'c'],
        },
        // Windows on exterior walls.
        {
          id: 'a-south-window', from: [1, -4], to: [3, -4], kind: 'window',
          opening: { width: 1.3, bottom: 1.0, height: 1.5 },
        },
        {
          id: 'b-east-window', from: [8, -4], to: [8, -2], kind: 'window',
          opening: { width: 1.3, bottom: 1.0, height: 1.5 },
        },
        // Notch edges left behind by removing the hall's NE corner cells:
        // overridden open so no straight wall crosses the rounded apse.
        { id: 'hall-notch-west', from: [6, 4], to: [6, 6], kind: 'open' },
        { id: 'hall-notch-south', from: [6, 4], to: [8, 4], kind: 'open' },
        // Host arch for the tower's radial throat, cut into the hall's west
        // exterior wall (exterior edges connect to 'outside' per the
        // compiler's adjacency rule; the throat itself, not this host cut,
        // is what actually joins tower and hall in the room graph).
        {
          id: 'tower-host', from: [0, 2], to: [0, 4], kind: 'arch',
          connects: ['hall', 'outside'],
          opening: { offset: 0.35, width: 1.3, bottom: 0, height: 2.6 },
        },
      ],
    },
  ],
  curves: [
    // Quarter-curve apse rounding the hall's north-east corner, tangent-
    // joined to the east wall (x=8, z<4) at its E end and the north wall
    // (z=6, x<6) at its N end.
    {
      id: 'hall-apse', levelId: 'ground', roomId: 'hall', kind: 'quarter',
      center: [6, 4], radius: 2, start: 'E', end: 'N', requireGridJoin: true,
      thickness: 0.6, height: 4, material: 'limestone', bond: 'ashlar',
      apertures: [
        { id: 'apse-window', kind: 'window', startAngle: 30, endAngle: 60, bottom: 1.0, height: 1.4 },
      ],
    },
    // Ring tower with a radial throat into the hall, plus three windows.
    {
      id: 'tower-ring', levelId: 'ground', roomId: 'tower', kind: 'ring',
      center: [-3, 3], radius: 3,
      thickness: 0.6, height: 4, material: 'limestone', bond: 'ashlar',
      apertures: [
        {
          id: 'tower-throat', kind: 'arch', startAngle: -16, endAngle: 16,
          bottom: 0, height: 2.6, connects: ['tower', 'hall'],
          throat: {
            targetRoomId: 'hall', direction: 'E', width: 1.3, depth: 0.6,
            hostApertureId: 'tower-host',
          },
        },
        { id: 'tower-window-e', kind: 'window', startAngle: 81, endAngle: 99, bottom: 1.1, height: 1.6 },
        { id: 'tower-window-n', kind: 'window', startAngle: 171, endAngle: 189, bottom: 1.1, height: 1.6 },
        { id: 'tower-window-w', kind: 'window', startAngle: 261, endAngle: 279, bottom: 1.1, height: 1.6 },
      ],
    },
  ],
  stairs: [],
  beams: [],
  fixtures: [],
  roofs: [],
  localLights: [],
};
