import { CASTLE_PLAN_SCHEMA } from '../../shared-lib/castle_plan.js';
import { CASTLE_SITE_SCHEMA } from '../../shared-lib/castle_site.js';

// The first frozen angled-site study. Portal opening offsets keep both authored
// mouth centers exact: core east [12,0,6], hall west [0,0,4].
export const ANGLED_STUDY_CORE_PLAN = Object.freeze({
  schema: CASTLE_PLAN_SCHEMA,
  id: 'angled-study-core',
  seed: 9411,
  entryRoomId: 'core-ground',
  style: { wallThickness: 0.6, wallMaterial: 'castle.limestone', bond: 'ashlar' },
  levels: [
    {
      id: 'ground', baseY: 0, height: 4,
      rooms: [{
        id: 'core-ground', use: 'great-hall', floorType: 'flags',
        rect: { x: 0, z: 0, width: 12, depth: 12 },
      }],
      edgeOverrides: [
        {
          id: 'main-entry', from: [0, 0], to: [0, 12], kind: 'door',
          connects: ['outside', 'core-ground'],
          opening: { width: 2.4, height: 3.2, offset: 4.8 },
        },
        {
          id: 'east-hall', from: [12, 0], to: [12, 12], kind: 'arch',
          connects: ['outside', 'core-ground'],
          opening: { width: 2.4, height: 3.2, offset: 4.8 },
        },
      ],
    },
    {
      id: 'upper', baseY: 4, height: 4,
      rooms: [{
        id: 'core-upper', use: 'solar', floorType: 'oak',
        rect: { x: 0, z: 0, width: 12, depth: 12 },
      }],
      edgeOverrides: [],
    },
  ],
  stairs: [{
    id: 'core-stair', lowerLevelId: 'ground', upperLevelId: 'upper',
    lowerRoomId: 'core-ground', upperRoomId: 'core-upper',
    width: 1.2, maxRiser: 0.2, tread: 0.25, headroom: 2.2,
    entryDirection: 'E', exitDirection: 'E',
    flights: [{
      id: 'straight-flight',
      footprint: { x: 3, z: 8.4, width: 5, depth: 1.2 },
      direction: 'E', stepCount: 20,
    }],
    landings: [
      { id: 'lower', kind: 'lower', bounds: { x: 1.8, z: 8.4, width: 1.2, depth: 1.2 } },
      { id: 'upper', kind: 'upper', bounds: { x: 8, z: 8.4, width: 1.2, depth: 1.2 } },
    ],
  }],
  beams: [], fixtures: [], roofs: [], localLights: [], curves: [],
});

export const ANGLED_STUDY_HALL_PLAN = Object.freeze({
  schema: CASTLE_PLAN_SCHEMA,
  id: 'angled-study-hall',
  seed: 9412,
  entryRoomId: 'hall-ground',
  style: { wallThickness: 0.6, wallMaterial: 'castle.limestone', bond: 'ashlar' },
  levels: [{
    id: 'ground', baseY: 0, height: 4,
    rooms: [{
      id: 'hall-ground', use: 'feasting-hall', floorType: 'flags',
      rect: { x: 0, z: 0, width: 14, depth: 8 },
    }],
    edgeOverrides: [{
      id: 'west-entry', from: [0, 0], to: [0, 8], kind: 'arch',
      connects: ['outside', 'hall-ground'],
      opening: { width: 2.4, height: 3.2, offset: 2.8 },
    }],
  }],
  stairs: [], beams: [], fixtures: [], roofs: [], localLights: [], curves: [],
});

export function angledStudySite(yawDeg = 30) {
  return {
    schema: CASTLE_SITE_SCHEMA,
    id: `angled-study-${yawDeg}`,
    seed: 9411,
    grid: 1,
    angleStep: 15,
    entry: { wing: 'core', level: 'ground', portal: 'main-entry' },
    wings: [
      { id: 'core', plan: ANGLED_STUDY_CORE_PLAN, frame: { origin: [0, 0, 0], yawDeg: 0 } },
      {
        id: 'hall', plan: ANGLED_STUDY_HALL_PLAN,
        placement: {
          socket: { level: 'ground', portal: 'west-entry' },
          relativeTo: { wing: 'core', level: 'ground', portal: 'east-hall' },
          yawDeg, outset: 6, lateral: 0,
        },
      },
    ],
    connections: [{
      id: 'vestibule',
      a: { wing: 'core', level: 'ground', portal: 'east-hall' },
      b: { wing: 'hall', level: 'ground', portal: 'west-entry' },
      floor: 'stone', height: 3.2,
      roof: { kind: 'low-hip', rise: 0.8 },
    }],
  };
}

export const CASTLE_SITE_ANGLED_STUDY = Object.freeze(angledStudySite(30));
