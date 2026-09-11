import { CASTLE_PLAN_SCHEMA } from '../../shared-lib/castle_plan.js';

// Small integration fixture intentionally authored out of lexical order. Tests
// reverse its arrays to prove that topology identity is input-order independent.
export const TWO_ROOM_TWO_LEVEL_PLAN = Object.freeze({
  schema: CASTLE_PLAN_SCHEMA,
  id: 'two-room-two-level',
  seed: 240911,
  entryRoomId: 'hall',
  style: {
    wallThickness: 0.6,
    wallMaterial: 'castle.limestone',
    bond: 'ashlar',
  },
  levels: [
    {
      id: 'upper', baseY: 4, height: 4,
      rooms: [
        { id: 'chamber', use: 'chamber', floorType: 'oak', rect: { x: 0, z: 0, width: 8, depth: 2 } },
      ],
      edgeOverrides: [],
    },
    {
      id: 'ground', baseY: 0, height: 4,
      rooms: [
        { id: 'hall', use: 'hall', floorType: 'flags', rect: { x: 0, z: 0, width: 8, depth: 2 } },
      ],
      edgeOverrides: [
        {
          id: 'front-door', from: [0, 0], to: [0, 2], kind: 'door',
          connects: ['outside', 'hall'], opening: { width: 1.2, height: 2.2, offset: 0.4 },
        },
      ],
    },
  ],
  stairs: [{
    id: 'main-stair', lowerLevelId: 'ground', upperLevelId: 'upper',
    lowerRoomId: 'hall', upperRoomId: 'chamber',
    width: 1.2, maxRiser: 0.2, tread: 0.25,
    headroom: 2.2,
    entryDirection: 'E', exitDirection: 'E',
    flights: [{
      id: 'flight-1',
      footprint: { x: 1.2, z: 0.4, width: 5, depth: 1.2 },
      direction: 'E',
      stepCount: 20,
    }],
    landings: [
      { id: 'lower', kind: 'lower', bounds: { x: 0, z: 0.4, width: 1.2, depth: 1.2 } },
      { id: 'upper', kind: 'upper', bounds: { x: 6.2, z: 0.4, width: 1.2, depth: 1.2 } },
    ],
  }],
  beams: [
    { levelId: 'upper', from: [0, 4, 2], to: [8, 4, 2], section: [0.2, 0.3], jointFamily: 'mortise-tenon', role: 'floor-beam' },
    { levelId: 'ground', from: [0, 0, 0], to: [0, 4, 0], section: [0.25, 0.25], jointFamily: 'pegged', role: 'post' },
  ],
  fixtures: [], roofs: [], localLights: [], curves: [],
});
