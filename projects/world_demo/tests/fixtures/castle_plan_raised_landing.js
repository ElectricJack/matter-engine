import { CASTLE_PLAN_SCHEMA } from '../../shared-lib/castle_plan.js';

// A straight two-flight stair whose turn landing sits above 2.1m, crossing the
// hall between its entry door and an annex door. The hall's flat route must
// detour around the flights and the posted landing: never beneath the landing,
// and never through the side band where the structure stands the flights'
// open-side parapets/balustrades and the landing's guard rails.
export function raisedLandingPlan() {
  return {
    schema: CASTLE_PLAN_SCHEMA, id: 'raised-landing-route', seed: 23, entryRoomId: 'hall',
    levels: [
      { id: 'ground', baseY: 0, height: 4, rooms: [
        { id: 'hall', use: 'hall', rect: { x: 0, z: 0, width: 10, depth: 6 } },
        { id: 'annex', use: 'pantry', rect: { x: 0, z: 6, width: 10, depth: 3 } },
      ], edgeOverrides: [
        { id: 'entry', from: [4, 0], to: [6, 0], kind: 'door', connects: ['outside', 'hall'],
          opening: { offset: 0.4, width: 1.2, bottom: 0, height: 2.2 } },
        { id: 'annex-door', from: [4, 6], to: [6, 6], kind: 'door', connects: ['hall', 'annex'],
          opening: { offset: 0.4, width: 1.2, bottom: 0, height: 2.2 } },
      ] },
      { id: 'upper', baseY: 4, height: 4, rooms: [
        { id: 'gallery', use: 'gallery', rect: { x: 0, z: 0, width: 10, depth: 6 } },
      ], edgeOverrides: [] },
    ],
    stairs: [{
      id: 'straight-stair', lowerLevelId: 'ground', upperLevelId: 'upper',
      lowerRoomId: 'hall', upperRoomId: 'gallery',
      width: 1.2, maxRiser: 0.2, tread: 0.25, headroom: 2.2,
      flights: [
        { id: 'lower-flight', direction: 'E', stepCount: 11,
          footprint: { x: 1.65, z: 2.4, width: 2.75, depth: 1.2 } },
        { id: 'upper-flight', direction: 'E', stepCount: 9,
          footprint: { x: 5.6, z: 2.4, width: 2.25, depth: 1.2 } },
      ],
      landings: [
        { id: 'lower', kind: 'lower', bounds: { x: 0.45, z: 2.4, width: 1.2, depth: 1.2 } },
        { id: 'turn', kind: 'intermediate', elevation: 2.2,
          bounds: { x: 4.4, z: 2.4, width: 1.2, depth: 1.2 } },
        { id: 'upper', kind: 'upper', bounds: { x: 7.85, z: 2.4, width: 1.2, depth: 1.2 } },
      ],
    }],
    beams: [], curves: [], fixtures: [], roofs: [], localLights: [],
  };
}
