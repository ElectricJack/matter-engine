import {
  authorRiverHydrologyNetwork,
  buildRiverHydrologyDefinition,
  buildRiverHydrologyField,
  riverHydrologyBiomes,
  sampleRiverHydrologyLane,
} from 'shared-lib/river_hydrology_definition';

export {
  authorRiverHydrologyNetwork,
  buildRiverHydrologyDefinition,
  buildRiverHydrologyField,
};

const RIVER_WATER = defineMaterial("RiverFloatLabWater", {
  albedo: [0.05, 0.14, 0.18],
  roughness: 0.06,
  transmission: 0.98,
  ior: 1.333,
  volumeBoundary: true,
  waterSurface: true,
});

function yawQuaternion(tangent) {
  const yaw = Math.atan2(tangent[2], tangent[0]);
  return [0, -Math.sin(yaw * 0.5), 0, Math.cos(yaw * 0.5)];
}

function rootLocalTransform(root) {
  const yaw = Math.atan2(root.transform[8], root.transform[0]);
  return {
    translation: [root.transform[3], root.transform[7], root.transform[11]],
    rotation: [0, -Math.sin(yaw * 0.5), 0, Math.cos(yaw * 0.5)],
    scale: [1, 1, 1],
  };
}

function bodyEntity(authored, placement, index) {
  const raft = placement.part === "RiverRaft";
  const halfExtents = raft ? [2.4, 0.35, 1.5] : [0.75, 0.75, 0.75];
  const lane = sampleRiverHydrologyLane(
    authored, placement.riverDistanceM, placement.lateralM);
  const bodyHeight = halfExtents[1] * 2;
  const surfaceY = lane.position[1] + lane.channel.depth;
  const equilibriumY = surfaceY + halfExtents[1] -
    (placement.densityKgM3 / 1000) * bodyHeight;
  const reference = placement.id === "reference-crate" ||
    placement.id === "reference-raft";
  const probes = raft ? (placement.probes || [3, 2, 3]) : [2, 2, 2];
  const variation = index % 4;
  const probeInset = raft ? 0.15 : 0.075;
  const maxForcePerProbe = raft ? 24000 : 5250;
  const maxTotalForce = raft ? 150000 : 35000;
  return {
    id: placement.id,
    name: placement.name,
    components: {
      LocalTransform: {
        translation: [lane.position[0], equilibriumY, lane.position[2]],
        rotation: yawQuaternion(lane.tangent),
        scale: [1, 1, 1],
      },
      PartInstance: { part: raft ? placement.part : "RiverCrate" },
      RigidBody: {
        type: "dynamic",
        linearDamping: 0.015 + variation * 0.005,
        angularDamping: 0.025 + variation * 0.01,
        gravityScale: 1,
        sleepThreshold: 0.05,
        enableSleep: !reference,
        continuous: reference,
      },
      BoxCollider: {
        halfExtents,
        density: placement.densityKgM3,
        friction: 0.42,
        restitution: 0.04,
      },
      RiverFloatBody: {
        effectiveDensityKgM3: placement.densityKgM3,
        displacedVolumeScale: 1,
        probesX: probes[0], probesY: probes[1], probesZ: probes[2],
        probeInset,
        buoyancyResponse: 1,
        longitudinalDrag: 0.72 + variation * 0.04,
        lateralDrag: 1.30 + variation * 0.08,
        verticalDrag: 1.65 + variation * 0.10,
        angularDamping: 0.34 + variation * 0.05,
        maxForcePerProbeN: maxForcePerProbe,
        maxTotalForceN: maxTotalForce,
        diagnosticColor: raft ? [0.25, 0.85, 0.95] : [0.95, 0.62, 0.20],
      },
    },
  };
}

function boulderColliderEntity(root) {
  return {
    id: `boulder-collider-${root.id}`,
    name: `${root.id} Static Collider`,
    components: {
      LocalTransform: rootLocalTransform(root),
      RigidBody: { type: "static", gravityScale: 1 },
      SphereCollider: {
        center: Array.from(root.fluidCollider.center),
        radius: root.fluidCollider.radius,
        density: 0,
        friction: 0.72,
        restitution: 0.02,
      },
    },
  };
}

function bodyPlacements(authored) {
  const lip = authored.waterfall.lipAt;
  const landing = authored.waterfall.landingAt;
  const spillway = authored.spillway.at;
  return [
    { id: "reference-raft", name: "Reference Raft", zone: "upper-rapids",
      riverDistanceM: 6, lateralM: 0, part: "RiverRaft", densityKgM3: 420,
      probes: [3, 2, 3] },
    { id: "reference-crate", name: "Reference Crate", zone: "upper-rapids",
      riverDistanceM: 16, lateralM: 0, part: "Crate", densityKgM3: 620,
      probes: [2, 2, 2] },

    { id: "upper-raft-01", name: "Upper Raft 01", zone: "upper-rapids",
      riverDistanceM: 28, lateralM: 2.4, part: "RiverRaft", densityKgM3: 390 },
    { id: "upper-crate-02", name: "Upper Crate 02", zone: "upper-rapids",
      riverDistanceM: 35, lateralM: -2.2, part: "Crate", densityKgM3: 570 },
    { id: "upper-crate-03", name: "Upper Crate 03", zone: "upper-rapids",
      riverDistanceM: 43, lateralM: 1.2, part: "Crate", densityKgM3: 690,
      probes: [3, 2, 2] },
    { id: "upper-raft-04", name: "Upper Raft 04", zone: "upper-rapids",
      riverDistanceM: 53, lateralM: -2.8, part: "RiverRaft", densityKgM3: 460 },
    { id: "upper-crate-05", name: "Upper Crate 05", zone: "upper-rapids",
      riverDistanceM: 64, lateralM: 2.0, part: "Crate", densityKgM3: 650 },
    { id: "upper-raft-06", name: "Upper Raft 06", zone: "upper-rapids",
      riverDistanceM: 76, lateralM: -1.4, part: "RiverRaft", densityKgM3: 430 },

    { id: "wake-crate-01", name: "Wake Crate 01", zone: "boulder-wakes",
      riverDistanceM: 31, lateralM: -3.1, part: "Crate", densityKgM3: 610 },
    { id: "wake-raft-02", name: "Wake Raft 02", zone: "boulder-wakes",
      riverDistanceM: 54, lateralM: 4.0, part: "RiverRaft", densityKgM3: 400 },
    { id: "wake-crate-03", name: "Wake Crate 03", zone: "boulder-wakes",
      riverDistanceM: 80, lateralM: -4.5, part: "Crate", densityKgM3: 720 },
    { id: "wake-raft-04", name: "Wake Raft 04", zone: "boulder-wakes",
      riverDistanceM: 98, lateralM: 2.8, part: "RiverRaft", densityKgM3: 480 },
    { id: "wake-crate-05", name: "Wake Crate 05", zone: "boulder-wakes",
      riverDistanceM: landing + 13, lateralM: -3.2, part: "Crate", densityKgM3: 590 },
    { id: "wake-raft-06", name: "Wake Raft 06", zone: "boulder-wakes",
      riverDistanceM: spillway + 22, lateralM: 2.5, part: "RiverRaft", densityKgM3: 450 },

    { id: "fall-crate-01", name: "Fall Crate 01", zone: "waterfall-approach",
      riverDistanceM: lip - 29, lateralM: -2.4, part: "Crate", densityKgM3: 560 },
    { id: "fall-raft-02", name: "Fall Raft 02", zone: "waterfall-approach",
      riverDistanceM: lip - 23, lateralM: 2.1, part: "RiverRaft", densityKgM3: 410 },
    { id: "fall-crate-03", name: "Fall Crate 03", zone: "waterfall-approach",
      riverDistanceM: lip - 17, lateralM: -1.0, part: "Crate", densityKgM3: 670 },
    { id: "fall-raft-04", name: "Fall Raft 04", zone: "waterfall-approach",
      riverDistanceM: lip - 11, lateralM: 1.6, part: "RiverRaft", densityKgM3: 370 },
    { id: "fall-crate-05", name: "Fall Crate 05", zone: "waterfall-approach",
      riverDistanceM: lip - 5, lateralM: 0.4, part: "Crate", densityKgM3: 740 },

    { id: "spill-raft-01", name: "Spillway Raft 01", zone: "first-spillway",
      riverDistanceM: landing + 9, lateralM: -3.0, part: "RiverRaft", densityKgM3: 440 },
    { id: "spill-crate-02", name: "Spillway Crate 02", zone: "first-spillway",
      riverDistanceM: landing + 18, lateralM: 2.8, part: "Crate", densityKgM3: 630 },
    { id: "spill-raft-03", name: "Spillway Raft 03", zone: "first-spillway",
      riverDistanceM: spillway - 22, lateralM: -2.0, part: "RiverRaft", densityKgM3: 500 },
    { id: "spill-crate-04", name: "Spillway Crate 04", zone: "first-spillway",
      riverDistanceM: spillway - 13, lateralM: 2.2, part: "Crate", densityKgM3: 600 },
    { id: "spill-raft-05", name: "Spillway Raft 05", zone: "first-spillway",
      riverDistanceM: spillway - 5, lateralM: 0.0, part: "RiverRaft", densityKgM3: 420 },
  ];
}

export function buildRiverFloatLabDefinition(worldSeed) {
  const river = buildRiverHydrologyDefinition(worldSeed);
  const placements = bodyPlacements(river);
  const entities = placements.map((placement, index) =>
    bodyEntity(river, placement, index));
  for (const root of river.roots) entities.push(boulderColliderEntity(root));
  entities.push({
    id: "river-player",
    name: "River Player",
    components: {
      LocalTransform: {
        translation: [48, 126, 31],
        rotation: [0, 0, 0, 1],
        scale: [1, 1, 1],
      },
      CharacterController: {
        radius: 0.4, height: 1.8, moveSpeed: 4.5,
        maxSlopeAngleDeg: 45, stepHeight: 0.45, jumpSpeed: 5,
      },
    },
  });
  return Object.freeze({
    river,
    roots: river.roots,
    bodyPlacements: Object.freeze(placements.map(row => Object.freeze(row))),
    entities: Object.freeze(entities),
  });
}

class RiverFloatLab extends World {
  static world = { sectorSize: 64, yMin: -48, yMax: 144 };
  static camera = { position: [48, 91, 31], target: [31, 69, 7] };
  static volumetrics = { enabled: false };
  static streaming = {
    nestedSectors: true, volumetricSectors: true,
    terrainBands: [
      { radius: 128, lod: 5 }, { radius: 256, lod: 4 },
      { radius: 448, lod: 3 }, { radius: 704, lod: 2 },
    ],
  };
  static roots = buildRiverHydrologyDefinition(0).roots;

  collision() {
    const collision = terrainCollision({
      cellSize: 0.5,
      friction: 0.72,
      restitution: 0.02,
    });
    collision.region("river-gameplay", {
      min: [-64, -64, -64],
      max: [384, 128, 64],
    });
    collision.build();
  }

  hydrology() {
    authorRiverHydrologyNetwork(this.worldSeed, RIVER_WATER);
  }

  field(p) {
    return buildRiverHydrologyField(p);
  }

  biomes() {
    return riverHydrologyBiomes();
  }

  buildEntities() {
    const definition = buildRiverFloatLabDefinition(this.worldSeed);
    for (const entity of definition.entities) this.entity(entity);
  }
}
