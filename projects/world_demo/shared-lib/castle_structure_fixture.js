// Structural-geometry fixture for the castle kit: a two-level keep with a
// hall/pantry pair, a circular guard tower joined to the hall by a radial
// throat (and its twin room stacked above), an L-shaped quarter-turn stair
// with an intermediate landing (its lower landing faces the entry door, so the
// compiled hall route approaches it without crossing either flight), a full oak-frame bay (support beam, posts,
// knee braces, roof ties), and pass-through gable/hip/conical roof records.
// It exercises rooms, edgeOverrides, curves/rings, radialThroats, stairs,
// beams, floorStructure, and roofs together in one compiled manifest.
import { CASTLE_PLAN_SCHEMA, compilePlan } from 'shared-lib/castle_plan';

// Compiled id of the mid-span floor-beam below, computed the same way the
// compiler derives beam ids (see castle_plan.js buildBeamMembers): the
// support beam is authored on levelId 'upper' even though its Y coordinate
// (3.51m) sits physically under the upper floor, because
// floorStructure.intermediateSupportIds is only accepted when the beam's
// levelId matches the floor's levelId (chamber's floor is on 'upper').
const SUPPORT_BEAM_ID = 'beam:upper:7.5,3.51,0|7.5,3.51,4.2:floor-beam';

export const CASTLE_STRUCTURE_FIXTURE_PLAN = Object.freeze({
  schema: CASTLE_PLAN_SCHEMA,
  id: 'castle-structure-fixture',
  seed: 91107,
  entryRoomId: 'hall',
  style: {
    wallThickness: 0.6,
    wallMaterial: 'castle.limestone',
    bond: 'ashlar',
  },
  levels: [
    {
      id: 'ground', baseY: 0, height: 4,
      rooms: [
        { id: 'hall', use: 'hall', floorType: 'flags', rect: { x: 0, z: 0, width: 10, depth: 6 } },
        { id: 'store', use: 'pantry', floorType: 'flags', rect: { x: -4, z: 1, width: 4, depth: 4 } },
        {
          id: 'tower', use: 'guardroom', floorType: 'flags',
          boundary: { kind: 'circle', center: [13, 3], radius: 3 },
        },
      ],
      edgeOverrides: [
        {
          id: 'hall-outside-door', from: [8, 0], to: [10, 0], kind: 'door',
          connects: ['outside', 'hall'],
          opening: { width: 1.2, height: 2.2, offset: 0.4 },
        },
        {
          id: 'hall-store-door', from: [0, 1], to: [0, 5], kind: 'door',
          connects: ['hall', 'store'],
          opening: { width: 1.4, height: 2.2, offset: 1.3 },
        },
        {
          // Host aperture for the ground-floor radial throat below. It is
          // authored with connects:['hall','outside'] (mirroring the
          // reference roundkeep plan) but is suppressed from becoming an
          // ordinary outside portal once the throat claims it.
          id: 'hall-tower-door', from: [10, 2], to: [10, 4], kind: 'door',
          connects: ['hall', 'outside'],
          opening: { width: 1.4, height: 2.2, offset: 0.3 },
        },
      ],
    },
    {
      id: 'upper', baseY: 4, height: 4,
      rooms: [
        {
          id: 'chamber', use: 'chamber', floorType: 'oak',
          rect: { x: 0, z: 0, width: 10, depth: 6 },
          floorStructure: {
            joistDirection: 'x', joistSpacing: 0.5,
            intermediateSupportIds: [SUPPORT_BEAM_ID],
          },
        },
        {
          id: 'tower-top', use: 'chamber', floorType: 'oak',
          boundary: { kind: 'circle', center: [13, 3], radius: 3 },
        },
      ],
      edgeOverrides: [
        {
          // Host aperture for the upper-floor radial throat.
          id: 'chamber-towertop-door', from: [10, 2], to: [10, 4], kind: 'door',
          connects: ['chamber', 'outside'],
          opening: { width: 1.4, height: 2.2, offset: 0.3 },
        },
      ],
    },
  ],
  curves: [
    {
      id: 'ground-tower-ring', levelId: 'ground', roomId: 'tower', kind: 'ring',
      center: [13, 3], radius: 3,
      apertures: [{
        id: 'ground-tower-throat', kind: 'door', startAngle: 160, endAngle: 200,
        bottom: 0, height: 2.2, connects: ['tower', 'hall'],
        throat: { targetRoomId: 'hall', direction: 'W', width: 1.4, depth: 1.2 },
      }],
    },
    {
      id: 'upper-tower-ring', levelId: 'upper', roomId: 'tower-top', kind: 'ring',
      center: [13, 3], radius: 3,
      apertures: [{
        id: 'upper-tower-throat', kind: 'door', startAngle: 160, endAngle: 200,
        bottom: 0, height: 2.2, connects: ['tower-top', 'chamber'],
        throat: { targetRoomId: 'chamber', direction: 'W', width: 1.4, depth: 1.2 },
      }],
    },
  ],
  stairs: [{
    id: 'main-stair',
    lowerLevelId: 'ground', upperLevelId: 'upper',
    lowerRoomId: 'hall', upperRoomId: 'chamber',
    width: 1.2, tread: 0.28, maxRiser: 0.2, headroom: 2.2,
    flights: [
      {
        id: 'flight-west', direction: 'W', stepCount: 10,
        footprint: { x: 5.6, z: 4.4, width: 2.8, depth: 1.2 },
      },
      {
        id: 'flight-south', direction: 'S', stepCount: 10,
        footprint: { x: 4.4, z: 1.6, width: 1.2, depth: 2.8 },
      },
    ],
    landings: [
      { id: 'lower', kind: 'lower', bounds: { x: 8.4, z: 4.4, width: 1.2, depth: 1.2 } },
      { id: 'turn', kind: 'intermediate', bounds: { x: 4.4, z: 4.4, width: 1.2, depth: 1.2 }, elevation: 2 },
      { id: 'upper', kind: 'upper', bounds: { x: 4.4, z: 0.4, width: 1.2, depth: 1.2 } },
    ],
  }],
  beams: [
    {
      levelId: 'upper', from: [7.5, 3.51, 0], to: [7.5, 3.51, 4.2],
      section: [0.3, 0.3], jointFamily: 'mortise-tenon', role: 'floor-beam', material: 'oak',
    },
    {
      levelId: 'ground', from: [7.5, 0, 2], to: [7.5, 3.36, 2],
      section: [0.25, 0.25], jointFamily: 'pegged', role: 'post', material: 'oak',
    },
    {
      levelId: 'ground', from: [7.5, 0, 4.15], to: [7.5, 3.36, 4.15],
      section: [0.25, 0.25], jointFamily: 'pegged', role: 'post', material: 'oak',
    },
    {
      levelId: 'ground', from: [7.5, 2.61, 2], to: [7.5, 3.36, 1.25],
      section: [0.14, 0.14], jointFamily: 'pegged', role: 'brace', material: 'oak',
    },
    {
      levelId: 'ground', from: [7.5, 2.61, 2], to: [7.5, 3.36, 2.75],
      section: [0.14, 0.14], jointFamily: 'pegged', role: 'brace', material: 'oak',
    },
    {
      levelId: 'ground', from: [7.5, 2.61, 4.15], to: [7.5, 3.36, 3.4],
      section: [0.14, 0.14], jointFamily: 'pegged', role: 'brace', material: 'oak',
    },
    {
      levelId: 'upper', from: [0, 8.18, 0], to: [0, 8.18, 6],
      section: [0.28, 0.36], jointFamily: 'mortise-tenon', role: 'roof-tie', material: 'oak',
    },
    {
      levelId: 'upper', from: [3.3, 8.18, 0], to: [3.3, 8.18, 6],
      section: [0.28, 0.36], jointFamily: 'mortise-tenon', role: 'roof-tie', material: 'oak',
    },
    {
      levelId: 'upper', from: [6.7, 8.18, 0], to: [6.7, 8.18, 6],
      section: [0.28, 0.36], jointFamily: 'mortise-tenon', role: 'roof-tie', material: 'oak',
    },
    {
      levelId: 'upper', from: [10, 8.18, 0], to: [10, 8.18, 6],
      section: [0.28, 0.36], jointFamily: 'mortise-tenon', role: 'roof-tie', material: 'oak',
    },
  ],
  roofs: [
    {
      id: 'chamber-roof', levelId: 'upper', kind: 'gable',
      bounds: { x: 0, z: 0, width: 10, depth: 6 }, baseY: 8, rise: 3.5,
      ridgeAxis: 'x', overhang: 0.45, material: 'slate', timberMaterial: 'oak',
    },
    {
      id: 'store-roof', levelId: 'ground', kind: 'hip',
      bounds: { x: -4, z: 1, width: 4, depth: 4 }, baseY: 4, rise: 2,
      ridgeAxis: 'z', overhang: 0.45, material: 'terracotta', timberMaterial: 'oak',
    },
    {
      id: 'tower-roof', levelId: 'upper', kind: 'conical',
      center: [13, 3], radius: 3, baseY: 8, rise: 5.5,
      overhang: 0.5, material: 'slate', timberMaterial: 'oak',
    },
  ],
  fixtures: [],
  localLights: [],
  verticalVoids: [],
});

let cached = null;
export function castleStructureFixtureManifest() {
  return cached || (cached = compilePlan(CASTLE_STRUCTURE_FIXTURE_PLAN));
}
