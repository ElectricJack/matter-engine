import { riverCurve, sampleRiverCurve } from 'shared-lib/river_curve';

const BOULDER_SPECS = [
  { id: "rock-01", at: 24, lateral: -3.0, size: 2.7, seed: 41 },
  { id: "rock-02", at: 47, lateral:  4.5, size: 4.0, seed: 42 },
  { id: "rock-03", at: 73, lateral: -5.0, size: 3.1, seed: 43 },
  { id: "rock-04", at: 92, lateral:  2.0, size: 3.5, seed: 44 },
];

export function channelProfileAt(profile, distance) {
  if (distance <= profile[0].at) return profile[0];
  for (let i = 1; i < profile.length; ++i) {
    const previous = profile[i - 1];
    const next = profile[i];
    if (distance <= next.at) {
      const t = (distance - previous.at) / (next.at - previous.at);
      return {
        width: previous.width + (next.width - previous.width) * t,
        depth: previous.depth + (next.depth - previous.depth) * t,
        asymmetry: previous.asymmetry +
          (next.asymmetry - previous.asymmetry) * t,
      };
    }
  }
  return profile[profile.length - 1];
}

function roundedV(t) {
  const clamped = Math.min(1, Math.max(0, t));
  const roundness = 0.08;
  const denominator = Math.sqrt(1 + roundness * roundness) - roundness;
  return (Math.sqrt(clamped * clamped + roundness * roundness) - roundness) /
    denominator;
}

function roundedVSlope(t) {
  const clamped = Math.min(1, Math.max(0, t));
  const roundness = 0.08;
  const denominator = Math.sqrt(1 + roundness * roundness) - roundness;
  return clamped /
    (Math.sqrt(clamped * clamped + roundness * roundness) * denominator);
}

function boulderRoot(spec, curve, channelProfile) {
  const sample = sampleRiverCurve(curve, spec.at);
  const channel = channelProfileAt(channelProfile, spec.at);
  const halfWidth = channel.width * 0.5;
  const lateralFraction = Math.abs(spec.lateral) / halfWidth;
  const signedAsymmetry = spec.lateral >= 0 ?
    channel.asymmetry : -channel.asymmetry;
  const bankRise = channel.depth * (1 + 0.85 * signedAsymmetry);
  const terrainY = sample.position[1] + bankRise * roundedV(lateralFraction);
  const x = sample.position[0] + sample.lateral[0] * spec.lateral;
  const y = terrainY + spec.size * 0.15;
  const z = sample.position[2] + sample.lateral[2] * spec.lateral;
  const yaw = Math.atan2(sample.tangent[2], sample.tangent[0]);
  const c = Math.cos(yaw);
  const s = Math.sin(yaw);
  const colliderRadius = spec.size * 0.45;
  const lateralSlope = bankRise * roundedVSlope(lateralFraction) / halfWidth;
  const horizontalTangent = Math.hypot(sample.tangent[0], sample.tangent[2]);
  const longitudinalSlope = sample.tangent[1] / horizontalTangent;
  const colliderWorldLift = colliderRadius * Math.sqrt(
    1 + lateralSlope * lateralSlope + longitudinalSlope * longitudinalSlope) +
    0.25;
  const colliderLocalY = colliderWorldLift - spec.size * 0.15;
  return {
    id: spec.id,
    module: "Rock",
    params: { seed: spec.seed, size: spec.size, detail: 1.0 },
    transform: Object.freeze([
      c, 0, -s, x,
      0, 1, 0, y,
      s, 0, c, z,
      0, 0, 0, 1,
    ]),
    fluidCollider: Object.freeze({
      shape: "sphere",
      radius: colliderRadius,
      center: Object.freeze([0, colliderLocalY, 0]),
    }),
  };
}

// This is the one pure source of the accepted ravine, sequential section,
// waterfall, pool/spillway, and boulder definition used by both visual worlds.
export function buildRiverHydrologyDefinition(worldSeed) {
  const path = riverCurve([0, 72, 0], { maxSegmentLength: 0.5 });
  path.cubicTo([32, 67, 18], [70, 61, -22], [106, 56, 8]);
  const waterfallLip = path.distance();
  path.lineTo([111, 44, 5]);
  const waterfallLanding = path.distance();
  path.cubicTo([121, 44, 2], [136, 44, -3], [148, 44, 0]);
  const firstSpillway = path.distance();
  path.cubicTo([184, 38, -24], [222, 31, 28], [258, 24, 4]);
  const secondPoolApproach = path.distance();
  path.cubicTo([270, 21.666667, -4], [282, 22, -2], [294, 22, 0]);
  const secondSpillway = path.distance();
  const mainCurve = path.build();

  const boulderSpecs = BOULDER_SPECS.concat([
    { id: "rock-05", at: waterfallLanding + 8, lateral: -4.0, size: 4.2, seed: 45 },
    { id: "rock-06", at: firstSpillway + 18, lateral:  3.0, size: 2.9, seed: 46 },
    { id: "rock-07", at: firstSpillway + 37, lateral: -5.5, size: 3.4, seed: 47 },
    { id: "rock-08", at: firstSpillway + 55, lateral:  6.0, size: 2.6, seed: 48 },
    { id: "rock-09", at: firstSpillway + 74, lateral: -2.0, size: 4.1, seed: 49 },
    { id: "rock-10", at: firstSpillway + 91, lateral:  4.0, size: 3.0, seed: 50 },
    { id: "rock-11", at: secondSpillway - 31, lateral: -6.0, size: 3.7, seed: 51 },
    { id: "rock-12", at: secondSpillway - 14, lateral:  2.5, size: 2.8, seed: 52 },
  ]);

  const channelProfile = [
    { at: 0, width: 14, depth: 8.0, asymmetry: 0.18 },
    { at: waterfallLip - 34, width: 24, depth: 8.5, asymmetry: -0.14 },
    { at: waterfallLip, width: 18, depth: 7.5, asymmetry: 0.10 },
    { at: waterfallLanding, width: 26, depth: 8.0, asymmetry: -0.08 },
    { at: waterfallLanding + 12, width: 34, depth: 8.0, asymmetry: 0.05 },
    { at: firstSpillway - 12, width: 34, depth: 8.0, asymmetry: 0.05 },
    { at: firstSpillway, width: 10, depth: 6.0, asymmetry: 0.0 },
    { at: firstSpillway + 12, width: 16, depth: 7.5, asymmetry: -0.16 },
    { at: firstSpillway + 42, width: 22, depth: 8.0, asymmetry: 0.20 },
    { at: firstSpillway + 78, width: 28, depth: 8.5, asymmetry: -0.18 },
    { at: secondPoolApproach, width: 20, depth: 7.5, asymmetry: 0.12 },
    { at: secondSpillway - 24, width: 38, depth: 8.0, asymmetry: 0.0 },
    { at: secondSpillway, width: 10, depth: 6.0, asymmetry: 0.0 },
  ];

  return Object.freeze({
    worldSeed,
    curve: mainCurve,
    channelProfile,
    sections: Object.freeze([
      Object.freeze({ id: "upper", from: 0, to: firstSpillway,
                      length: firstSpillway }),
      Object.freeze({ id: "lower", from: firstSpillway, to: secondSpillway,
                      length: secondSpillway - firstSpillway }),
    ]),
    waterfall: Object.freeze({
      lipAt: waterfallLip,
      landingAt: waterfallLanding,
      drop: 12,
    }),
    firstPool: Object.freeze({
      from: waterfallLanding, to: firstSpillway, fillLevel: 50,
    }),
    secondPool: Object.freeze({
      from: secondSpillway - 22, to: secondSpillway, fillLevel: 28,
    }),
    spillway: Object.freeze({
      id: "pool-one", at: firstSpillway, width: 10,
      effectiveDepth: 6, overlap: 5, damOffset: 4,
    }),
    finalSpillway: Object.freeze({
      id: "pool-two", at: secondSpillway, width: 10,
      effectiveDepth: 6, overlap: 5, damOffset: 4,
    }),
    roots: Object.freeze(boulderSpecs.map(spec =>
      Object.freeze(boulderRoot(spec, mainCurve, channelProfile)))),
  });
}

export function sampleRiverHydrologyLane(authored, distance, lateral = 0) {
  const sample = sampleRiverCurve(authored.curve, distance);
  const channel = channelProfileAt(authored.channelProfile, distance);
  return {
    position: [
      sample.position[0] + sample.lateral[0] * lateral,
      sample.position[1],
      sample.position[2] + sample.lateral[2] * lateral,
    ],
    tangent: sample.tangent,
    lateral: sample.lateral,
    channel,
    distance: sample.distance,
  };
}

// These delegates keep the two worlds on the same accepted simulation and
// terrain inputs without moving gameplay entities into the river generator.
export function authorRiverHydrologyNetwork(worldSeed) {
  const authored = buildRiverHydrologyDefinition(worldSeed);
  const fixedStep = 1 / 120;
  const maxSteps = 8192;
  const network = riverNetwork({
    cellSize: 0.5,
    seed: worldSeed ^ 0x52495645,
  });
  const main = network.river("main")
    .inlet(authored.curve[0], { flow: 600.0 })
    .curve(authored.curve)
    .channelProfile(authored.channelProfile);

  network.backend("physx");
  network.pbd({
    particleSpacing: 0.20,
    restDensity: 1000,
    fixedStep,
    iterations: 4,
    maxNeighbors: 96,
  });
  network.limits({ batchSteps: 256, maxSteps, maxParticles: 4000000 });
  network.escapePolicy({ absoluteCount: 32, ratio: 0.0001 });
  network.emitter({
    id: "upstream-inlet",
    position: [0, 84, 0],
    direction: [0.86, -0.14, 0.49],
    initialVelocity: [1.72, -0.28, 0.98],
    flow: 600.0,
    radius: 4.472136,
    startTime: 0,
    stopTime: maxSteps * fixedStep,
  });
  network.virtualDam({ height: 8, thickness: 0.5 });
  network.fillSensor({
    upstreamOffset: 2,
    length: 8,
    height: 6,
    resolution: [6, 1, 3],
    crestWetFraction: 0.80,
    stableWetSteps: 32,
    minimumParticlesPerCell: 1,
  });
  network.quality({
    particleRadius: 0.13,
    visualVoxel: 0.15,
    visualBlendWidth: 0.10,
    coarseVoxel: 0.65,
    gameplayCell: 0.50,
    maxVisualParticles: 1000000,
    maxGridVertices: 4194304,
    maxMeshVertices: 12582912,
    maxMeshIndices: 12582912,
  });

  main.section("upper", {
    from: authored.sections[0].from,
    to: authored.sections[0].to,
    dryMargin: 15,
  })
    .emitters(["upstream-inlet"])
    .waterfall({
      lipAt: authored.waterfall.lipAt,
      landingAt: authored.waterfall.landingAt,
      expectedDrop: authored.waterfall.drop,
    })
    .pool(authored.firstPool)
    .spillway(authored.spillway);
  main.section("lower", {
    from: authored.sections[1].from,
    to: authored.sections[1].to,
    dryMargin: 15,
  })
    .after("upper")
    .fromSpillway("upper")
    .pool(authored.secondPool)
    .spillway(authored.finalSpillway);
  network.bakeSequential();
  network.build();
}

export function buildRiverHydrologyField(p) {
  const grade = worldX().mul(-0.15).add(78.0);
  const broad = noise2(p.worldSeed ^ 0x31, 1 / 190, 4).mul(22.0);
  const ridges = ridge2(p.worldSeed ^ 0x51, 1 / 80, 3, 0.52, 2.0)
    .add(1).mul(0.5).pow(1.75).mul(30.0);
  return {
    density: heightToDensity(grade.add(broad).add(ridges)),
    moisture: noise2(p.worldSeed ^ 0x91, 1 / 90, 2),
    relief: noise2(p.worldSeed ^ 0x92, 1 / 120, 2),
    seaLevel: -100.0,
  };
}

export function riverHydrologyBiomes() {
  return {
    __terrain: { material: "dirt" },
    foothills: {}, meadow: {}, mountains: {}, ocean: {},
  };
}
