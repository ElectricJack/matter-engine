// Coarse, renderer-independent collision for the castle kit. No per-brick bodies.
//
// castleCollisionEntities(manifest, {prefix, offset}) returns ordinary World
// entity records. The array's non-enumerable `diagnostics` property describes
// approximations; invalid/unsupported records throw with their source id.
// Balcony void-edge rails are included. Furniture, doors, glazing, stair rails
// and roofs need their own colliders if they should constrain movement. Arch apertures retain
// their authored rectangular clearance until the kit supplies an arch profile.
const EPS = 1e-8;
const CURVE_CHORD = 0.25;
const CIRCLE_STRIP = 0.125;
const DIRECTIONS = { E: [1, 0], N: [0, 1], W: [-1, 0], S: [0, -1] };

function fail(id, message) {
  throw new Error(`castle collision ${id}: ${message}`);
}
function number(value, id) {
  if (typeof value !== "number" || !Number.isFinite(value))
    fail(id, "expected finite number");
  return value;
}
function positive(value, id) {
  if (number(value, id) <= 0) fail(id, "expected positive extent");
  return value;
}
function vector(value, size, id) {
  if (!Array.isArray(value) || value.length !== size)
    fail(id, `expected ${size} coordinates`);
  return value.map((v) => number(v, id));
}
function rect(value, id) {
  if (!value) fail(id, "missing rectangle");
  const x0 = number(value.x ?? value.minX, id);
  const z0 = number(value.z ?? value.minZ, id);
  const x1 =
    value.width === undefined
      ? number(value.maxX, id)
      : x0 + positive(value.width, id);
  const z1 =
    value.depth === undefined
      ? number(value.maxZ, id)
      : z0 + positive(value.depth, id);
  if (x1 <= x0 || z1 <= z0) fail(id, "empty rectangle");
  return [x0, z0, x1, z1];
}
function same(a, b) {
  return Math.abs(a - b) < EPS;
}
function key(values) {
  return values.map((v) => Math.round(v * 1e8)).join(",");
}

// Exact disjoint rectangle difference. The second coordinate is Z for floors
// and Y for a wall's elevation plane.
function subtract(a, b) {
  const x0 = Math.max(a[0], b[0]),
    z0 = Math.max(a[1], b[1]);
  const x1 = Math.min(a[2], b[2]),
    z1 = Math.min(a[3], b[3]);
  if (x1 <= x0 + EPS || z1 <= z0 + EPS) return [a];
  const out = [];
  if (a[0] < x0 - EPS) out.push([a[0], a[1], x0, a[3]]);
  if (x1 < a[2] - EPS) out.push([x1, a[1], a[2], a[3]]);
  if (a[1] < z0 - EPS) out.push([x0, a[1], x1, z0]);
  if (z1 < a[3] - EPS) out.push([x0, z1, x1, a[3]]);
  return out;
}
function cut(rectangles, holes) {
  let out = rectangles;
  for (const hole of holes) out = out.flatMap((r) => subtract(r, hole));
  return out;
}
function merge(rectangles) {
  let out = [...new Map(rectangles.map((r) => [key(r), r])).values()];
  for (let iteration = 0; iteration < 4; ++iteration) {
    const before = out.length;
    for (const axis of [0, 1]) {
      const other = 1 - axis;
      const groups = new Map();
      for (const r of out) {
        const k = key([r[other], r[other + 2]]);
        if (!groups.has(k)) groups.set(k, []);
        groups.get(k).push([...r]);
      }
      out = [];
      for (const group of groups.values()) {
        group.sort((a, b) => a[axis] - b[axis]);
        let last;
        for (const r of group) {
          if (last && r[axis] <= last[axis + 2] + EPS)
            last[axis + 2] = Math.max(last[axis + 2], r[axis + 2]);
          else {
            last = r;
            out.push(r);
          }
        }
      }
    }
    if (out.length === before) break;
  }
  return out.sort(
    (a, b) => a[0] - b[0] || a[1] - b[1] || a[2] - b[2] || a[3] - b[3],
  );
}

/** Static box collision from a compiled castle manifest; throws on ambiguity
 * that would block a passage or remove an unsupported amount of floor. */
export function castleCollisionEntities(
  manifest,
  { prefix = "castle-collision", offset = [0, 0, 0] } = {},
) {
  if (!manifest || typeof manifest !== "object")
    fail("manifest", "expected object");
  if (typeof prefix !== "string" || !prefix)
    fail("prefix", "expected nonempty string");
  offset = vector(offset, 3, "offset");
  const entities = [],
    diagnostics = [],
    notices = new Set(),
    shapeKeys = new Set();
  const levels = new Map(
    (manifest.levels || []).map((l) => [l.id, number(l.baseY, l.id)]),
  );
  const rooms = new Map((manifest.rooms || []).map((r) => [r.id, r]));
  function note(code, recordId, message) {
    const k = `${code}:${recordId}`;
    if (!notices.has(k)) {
      notices.add(k);
      diagnostics.push({ code, recordId, message });
    }
  }
  function baseY(record) {
    if (!levels.has(record.levelId))
      fail(record.id, `unknown level ${record.levelId}`);
    return levels.get(record.levelId);
  }
  function box(sourceId, center, size, yaw = 0) {
    center = vector(center, 3, sourceId);
    size = vector(size, 3, sourceId);
    if (size.some((v) => v <= EPS)) fail(sourceId, "degenerate box");
    number(yaw, sourceId);
    const translation = center.map((v, i) => number(v + offset[i], sourceId));
    const shapeKey = key([...translation, ...size, yaw]);
    if (shapeKeys.has(shapeKey)) return;
    shapeKeys.add(shapeKey);
    entities.push({
      id: `${prefix}:${sourceId}:${entities.length}`,
      name: `Castle collision ${sourceId}`,
      components: {
        LocalTransform: {
          translation,
          rotation: [0, Math.sin(yaw / 2), 0, Math.cos(yaw / 2)],
        },
        RigidBody: { type: "static" },
        BoxCollider: { halfExtents: size.map((v) => v / 2) },
      },
    });
  }
  function slab(id, r, top, thickness) {
    box(
      id,
      [(r[0] + r[2]) / 2, top - thickness / 2, (r[1] + r[3]) / 2],
      [r[2] - r[0], thickness, r[3] - r[1]],
    );
  }

  // Resolve exact stair holes before reading old compiler floor.holes records,
  // which can contain a bounding rectangle rather than the union of flights.
  const voidRegions = new Map(),
    stairFloorHoles = new Map(),
    landings = [];
  for (const stair of manifest.stairs || []) {
    for (const v of stair.voids || []) {
      if (v.kind === "floor" && Array.isArray(v.regions))
        voidRegions.set(
          v.id,
          v.regions.map((r) => rect(r, v.id)),
        );
    }
    for (const h of stair.holes || []) {
      if (!h.floorId) fail(stair.id, "stair hole lacks floorId");
      if (!stairFloorHoles.has(h.floorId)) stairFloorHoles.set(h.floorId, []);
      stairFloorHoles.get(h.floorId).push(rect(h.footprint ?? h.bounds, h.id));
    }
    for (const landing of stair.landings || [])
      landings.push({
        ...landing,
        collisionRect: rect(landing.bounds, landing.id),
        elevation: number(landing.elevation, landing.id),
        thickness: positive(
          landing.thickness ?? stair.landingThickness ?? 0.25,
          landing.id,
        ),
      });
  }

  function floorRegion(region, id) {
    if (!region || typeof region !== "object") fail(id, "missing floor region");
    if (region.kind === "cells") {
      if (!Array.isArray(region.cells)) fail(id, "cells boundary lacks cells");
      return merge(
        region.cells.map((c) => {
          const [x, z] = vector(c, 2, id);
          return [x, z, x + 1, z + 1];
        }),
      );
    }
    if (region.kind === "circle") {
      const [x, z] = vector(region.center, 2, id),
        radius = positive(region.radius, id);
      const count = Math.ceil((radius * 2) / CIRCLE_STRIP);
      if (count > 20000)
        fail(id, "circular floor exceeds collision strip budget");
      const out = [];
      for (let i = 0; i < count; ++i) {
        const lo = -radius + (i * radius * 2) / count;
        const hi = -radius + ((i + 1) * radius * 2) / count;
        const near =
          lo <= 0 && hi >= 0 ? 0 : Math.min(Math.abs(lo), Math.abs(hi));
        const half = Math.sqrt(Math.max(0, radius * radius - near * near));
        out.push([x - half, z + lo, x + half, z + hi]);
      }
      note(
        "CIRCULAR_FLOOR_BOX_APPROXIMATION",
        id,
        `Circular floor uses covering strips; boundary excess is at most ${CIRCLE_STRIP}m, normally inside its wall.`,
      );
      return out;
    }
    if (
      region.kind === "rect" ||
      region.kind === "rectangle" ||
      region.kind === "polygon-rect"
    )
      return [rect(region.bounds ?? region.rect ?? region, id)];
    fail(id, `unsupported floor region ${region.kind}`);
  }

  for (const floor of manifest.floors || []) {
    const room = rooms.get(floor.roomId);
    if (
      floor.openToBelow ||
      room?.openToBelow ||
      floor.walkable === false ||
      room?.walkable === false ||
      ["void", "air", "none"].includes(floor.floorType) ||
      ["void", "air"].includes(room?.use)
    )
      continue;
    const top = number(floor.elevation ?? baseY(floor), floor.id);
    const thickness = positive(floor.thickness ?? 0.25, floor.id);
    let regions = (floor.regions ?? [floor.boundary]).flatMap((r) =>
      floorRegion(r, floor.id),
    );
    // Extension patches must not create overlapping coplanar floor bodies.
    for (const extension of floor.extensions || []) {
      const additions = floorRegion(extension, floor.id);
      regions.push(...cut(additions, regions));
    }
    const holes = [...(stairFloorHoles.get(floor.id) || [])];
    for (const hole of floor.holes || []) {
      if (hole.kind === "ceiling") continue;
      if (!["floor", "stair", "double-height", "shaft"].includes(hole.kind))
        fail(
          floor.id,
          `hole ${hole.id ?? hole.voidId ?? ""} needs an explicit floor-attached kind`,
        );
      if (
        hole.replacementLandingId &&
        !landings.some((l) => l.id === hole.replacementLandingId)
      )
        fail(
          floor.id,
          `missing replacement landing ${hole.replacementLandingId}`,
        );
      if (voidRegions.has(hole.voidId))
        holes.push(...voidRegions.get(hole.voidId));
      else if (Array.isArray(hole.regions))
        holes.push(...hole.regions.map((r) => rect(r, floor.id)));
      else holes.push(rect(hole.footprint ?? hole.bounds, floor.id));
    }
    // Landing geometry owns its deck, including an upper landing replaced by
    // the stair emitter. Other floor regions, especially the lower deck, remain.
    for (const landing of landings)
      if (same(landing.elevation, top)) holes.push(landing.collisionRect);
    regions = merge(
      cut(regions, [...new Map(holes.map((h) => [key(h), h])).values()]),
    );
    for (const r of regions) slab(floor.id, r, top, thickness);
  }

  const wallById = new Map((manifest.walls || []).map((w) => [w.id, w]));
  const usedEdges = new Set();
  const wallRecords = [...(manifest.wallModules || [])];
  for (const module of wallRecords)
    for (const id of module.sourceEdgeIds || []) {
      if (usedEdges.has(id))
        fail(module.id, `wall edge ${id} belongs to multiple modules`);
      usedEdges.add(id);
    }
  wallRecords.push(
    ...(manifest.walls || []).filter((w) => !usedEdges.has(w.id)),
  );
  for (const wall of wallRecords) {
    if (wall.kind === "open") continue;
    const from = vector(wall.from, 2, wall.id),
      to = vector(wall.to, 2, wall.id);
    const dx = to[0] - from[0],
      dz = to[1] - from[1];
    if (Math.abs(dx) < EPS === Math.abs(dz) < EPS)
      fail(wall.id, "wall must be nonzero and axis aligned");
    const axis = Math.abs(dx) > EPS ? 0 : 1;
    const lo = Math.min(from[axis], to[axis]),
      hi = Math.max(from[axis], to[axis]);
    const y = baseY(wall),
      height = positive(wall.section?.height, wall.id);
    const thickness = positive(wall.section?.thickness, wall.id);
    let apertures = wall.apertures ?? wall.openings;
    if (!apertures)
      apertures = (wall.sourceEdgeIds || []).flatMap((id) => {
        if (!wallById.has(id)) fail(wall.id, `missing source edge ${id}`);
        return wallById.get(id).openings || [];
      });
    if (["door", "arch", "window"].includes(wall.kind) && !apertures.length)
      fail(wall.id, "opening wall has no aperture extents");
    const holes = [];
    for (const a of apertures) {
      let start, end;
      if (
        a.segmentFrom &&
        a.globalStart !== undefined &&
        a.globalEnd !== undefined
      ) {
        const origin = vector(a.segmentFrom, 2, wall.id)[axis];
        start = origin + number(a.globalStart, wall.id);
        end = origin + number(a.globalEnd, wall.id);
      } else {
        if ((wall.sourceEdgeIds || []).length > 1)
          fail(wall.id, "module aperture needs global coordinates");
        start = lo + number(a.localStart ?? a.start, wall.id);
        end = lo + number(a.localEnd ?? a.end, wall.id);
      }
      const bottom = number(a.bottom ?? 0, wall.id);
      const top = number(a.top ?? bottom + a.height, wall.id);
      if (end <= start || bottom < 0 || top <= bottom || top > height + EPS)
        fail(wall.id, "invalid aperture volume");
      holes.push([start, y + bottom, end, y + top]);
      if (a.kind === "arch")
        note(
          "ARCH_CLEARANCE_ENVELOPE",
          a.apertureId ?? wall.id,
          "Arch collider uses its authored rectangular clearance; curved crown masonry is not collision geometry.",
        );
    }
    for (const r of merge(cut([[lo, y, hi, y + height]], holes))) {
      const along = (r[0] + r[2]) / 2,
        cy = (r[1] + r[3]) / 2;
      box(
        wall.id,
        axis === 0 ? [along, cy, from[1]] : [from[0], cy, along],
        axis === 0
          ? [r[2] - r[0], r[3] - r[1], thickness]
          : [thickness, r[3] - r[1], r[2] - r[0]],
      );
    }
  }

  // A guarded open edge replaces a wall with a waist-high collision barrier.
  // Room-open and exterior-entry edges always remain traversable, even if a
  // stale rail flag survived a boundary edit.
  for (const boundary of manifest.openBoundaries || []) {
    if (
      boundary.kind === "room-open" ||
      boundary.kind === "exterior-entry" ||
      !(boundary.rail?.required === true || boundary.railRequired === true)
    )
      continue;
    if (boundary.kind !== "void-edge")
      fail(boundary.id, `unsupported guarded boundary ${boundary.kind}`);
    const rail = boundary.rail || {};
    const profile =
      typeof rail.profile === "object" && rail.profile !== null
        ? rail.profile
        : manifest.railProfiles?.[rail.profile] || {};
    const height = positive(
      rail.height ?? boundary.railHeight ?? profile.height ?? 1.1,
      boundary.id,
    );
    const thickness = positive(
      rail.thickness ?? boundary.railThickness ?? profile.thickness ?? 0.12,
      boundary.id,
    );
    const elevation = number(
      boundary.elevation ?? baseY(boundary),
      boundary.id,
    );
    if (!Array.isArray(boundary.hostEdgeIds) || !boundary.hostEdgeIds.length)
      fail(boundary.id, "guarded boundary needs hostEdgeIds");
    const groups = new Map();
    for (const edgeId of boundary.hostEdgeIds) {
      const edge = wallById.get(edgeId);
      if (!edge || edge.levelId !== boundary.levelId)
        fail(boundary.id, `missing host edge on this level: ${edgeId}`);
      if (edge.kind !== "open")
        fail(boundary.id, `guardrail host ${edgeId} is not an open edge`);
      const a = vector(edge.from, 2, boundary.id),
        b = vector(edge.to, 2, boundary.id);
      if (Math.abs(a[0] - b[0]) < EPS === Math.abs(a[1] - b[1]) < EPS)
        fail(boundary.id, "guardrail edge must be nonzero and axis aligned");
      const axis = same(a[1], b[1]) ? 0 : 1;
      const fixed = a[1 - axis],
        groupKey = `${axis}:${fixed}`;
      if (!groups.has(groupKey))
        groups.set(groupKey, { axis, fixed, spans: [] });
      groups
        .get(groupKey)
        .spans.push([
          Math.min(a[axis], b[axis]),
          0,
          Math.max(a[axis], b[axis]),
          1,
        ]);
    }
    for (const { axis, fixed, spans } of groups.values())
      for (const span of merge(spans)) {
        const center = (span[0] + span[2]) / 2,
          length = span[2] - span[0];
        box(
          boundary.id,
          axis === 0
            ? [center, elevation + height / 2, fixed]
            : [fixed, elevation + height / 2, center],
          axis === 0
            ? [length, height, thickness]
            : [thickness, height, length],
        );
      }
  }

  for (const curve of manifest.curves || []) {
    const [cx, cz] = vector(curve.center, 2, curve.id);
    const radius = positive(curve.radius, curve.id),
      thickness = positive(curve.section?.thickness, curve.id);
    const height = positive(curve.section?.height, curve.id),
      y = baseY(curve);
    let start = 0,
      end = 360;
    if (curve.kind === "quarter") {
      const endpoint = curve.endpoints?.[0]?.position;
      if (!endpoint) fail(curve.id, "quarter curve needs exact start endpoint");
      const p = vector(endpoint, 2, curve.id);
      start = (Math.atan2(p[1] - cz, p[0] - cx) * 180) / Math.PI;
      end = start + (curve.clockwise ? -90 : 90);
      if (start > end) [start, end] = [end, start];
    } else if (curve.kind !== "ring")
      fail(curve.id, `unsupported curve ${curve.kind}`);
    const openings = [];
    for (const a of curve.apertures || []) {
      const a0 = number(a.startAngle, curve.id),
        a1 = number(a.endAngle, curve.id);
      const bottom = number(a.bottom ?? 0, curve.id),
        top = bottom + positive(a.height, curve.id);
      if (a1 <= a0 || a1 - a0 > 360 || bottom < 0 || top > height + EPS)
        fail(curve.id, "invalid arc aperture");
      const shift = Math.floor((start - a0) / 360);
      for (let k = shift - 1; k <= shift + 2; ++k) {
        const lo = Math.max(start, a0 + k * 360),
          hi = Math.min(end, a1 + k * 360);
        if (hi > lo + EPS) openings.push([lo, bottom, hi, top]);
      }
      if (a.kind === "arch")
        note(
          "ARCH_CLEARANCE_ENVELOPE",
          a.id ?? curve.id,
          "Curved arch opening uses its angular clearance interval and authored height.",
        );
    }
    const cuts = [
      ...new Set([start, end, ...openings.flatMap((a) => [a[0], a[2]])]),
    ].sort((a, b) => a - b);
    for (let cutIndex = 0; cutIndex < cuts.length - 1; ++cutIndex) {
      const lo = cuts[cutIndex],
        hi = cuts[cutIndex + 1];
      const count = Math.ceil(
        ((((hi - lo) * Math.PI) / 180) * radius) / CURVE_CHORD,
      );
      if (count > 20000)
        fail(curve.id, "curve exceeds collision segment budget");
      for (let i = 0; i < count; ++i) {
        const a = ((lo + ((hi - lo) * i) / count) * Math.PI) / 180;
        const b = ((lo + ((hi - lo) * (i + 1)) / count) * Math.PI) / 180;
        const mid = (a + b) / 2,
          half = (b - a) / 2;
        const chordRadius = radius * Math.cos(half),
          sagitta = radius - chordRadius;
        const segmentLength = 2 * (radius + thickness / 2) * Math.sin(half);
        const center = [
          cx + chordRadius * Math.cos(mid),
          0,
          cz + chordRadius * Math.sin(mid),
        ];
        const angleMid = (mid * 180) / Math.PI;
        const active = openings.filter(
          (o) => angleMid > o[0] - EPS && angleMid < o[2] + EPS,
        );
        const bands = cut(
          [[0, 0, 1, height]],
          active.map((o) => [0, o[1], 1, o[3]]),
        );
        for (const band of bands)
          box(
            curve.id,
            [center[0], y + (band[1] + band[3]) / 2, center[2]],
            [segmentLength, band[3] - band[1], thickness + 2 * sagitta],
            -mid - Math.PI / 2,
          );
      }
    }
    note(
      "CURVED_WALL_BOX_APPROXIMATION",
      curve.id,
      `Curved wall uses chords no longer than ${CURVE_CHORD}m; boxes end at aperture boundaries.`,
    );
  }

  for (const stair of manifest.stairs || []) {
    if (!Array.isArray(stair.flights) || !stair.flights.length)
      fail(stair.id, "stairs need explicit flights");
    for (const flight of stair.flights) {
      const direction = DIRECTIONS[flight.direction];
      if (!direction) fail(flight.id, "unknown flight direction");
      const footprint = rect(flight.footprint, flight.id);
      const width = positive(flight.width ?? stair.width, flight.id);
      const tread = positive(flight.tread ?? stair.tread, flight.id);
      const riser = positive(flight.riser ?? stair.riser, flight.id);
      const count = positive(flight.stepCount, flight.id);
      if (!Number.isInteger(count))
        fail(flight.id, "stepCount must be integer");
      const fromY = number(flight.fromY, flight.id),
        toY = number(flight.toY, flight.id);
      if (!same(fromY + riser * count, toY))
        fail(flight.id, "riser count does not reach destination");
      const run = count * tread;
      const horizontal = direction[0] !== 0;
      if (
        (horizontal
          ? footprint[2] - footprint[0]
          : footprint[3] - footprint[1]) +
          EPS <
          run ||
        (horizontal
          ? footprint[3] - footprint[1]
          : footprint[2] - footprint[0]) +
          EPS <
          width
      )
        fail(flight.id, "flight geometry exceeds footprint");
      const thickness = positive(
        flight.treadThickness ?? stair.treadThickness ?? Math.min(riser, 0.2),
        flight.id,
      );
      const start = [
        (footprint[0] + footprint[2]) / 2 - (direction[0] * run) / 2,
        (footprint[1] + footprint[3]) / 2 - (direction[1] * run) / 2,
      ];
      for (let i = 0; i < count; ++i)
        box(
          `${flight.id}:tread:${i}`,
          [
            start[0] + direction[0] * (i + 0.5) * tread,
            fromY + (i + 1) * riser - thickness / 2,
            start[1] + direction[1] * (i + 0.5) * tread,
          ],
          horizontal ? [tread, thickness, width] : [width, thickness, tread],
        );
    }
  }
  for (const landing of landings)
    slab(
      landing.id,
      landing.collisionRect,
      landing.elevation,
      landing.thickness,
    );
  Object.defineProperty(entities, "diagnostics", {
    value: diagnostics,
    enumerable: false,
  });
  return entities;
}
