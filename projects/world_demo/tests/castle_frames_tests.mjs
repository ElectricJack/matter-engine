import assert from 'node:assert/strict';
import {
  composePlanarFrames,
  directionToVector,
  directionToYawDeg,
  inverseRotateXZ,
  inverseTransformPoint,
  inverseTransformPointXZ,
  inverseTransformVector,
  makePlanarFrame,
  multiplyQuaternions,
  multiplyTransforms,
  normalizeYawDeg,
  planarFrameMatrix,
  rotateXZ,
  solvePlanarFrame,
  transformAabbToObb,
  transformBoxCollider,
  transformDirection,
  transformEntity,
  transformHullCollider,
  transformHullPoints,
  transformPoint,
  transformPointXZ,
  transformQuaternion,
  transformRoot,
  transformSpotLight,
  transformVector,
  transformYawDeg,
  yawDegToDirection,
  yawDegToQuaternion,
} from '../shared-lib/castle_frames.js';

const EPSILON = 1e-10;

function near(actual, expected, message = '') {
  assert.equal(actual.length, expected.length, `${message} dimensionality`);
  actual.forEach((value, index) => assert.ok(
    Math.abs(value - expected[index]) <= EPSILON,
    `${message} component ${index}: expected ${expected[index]}, got ${value}`));
}

function quaternionNear(actual, expected, message = '') {
  const direct = actual.every((value, index) => Math.abs(value - expected[index]) <= EPSILON);
  const negated = actual.every((value, index) => Math.abs(value + expected[index]) <= EPSILON);
  assert.ok(direct || negated, `${message}: ${actual} is not rotation-equivalent to ${expected}`);
}

function rotateByQuaternion(vector, quaternion) {
  const [x, y, z, w] = quaternion;
  const [vx, vy, vz] = vector;
  const tx = 2 * (y * vz - z * vy);
  const ty = 2 * (z * vx - x * vz);
  const tz = 2 * (x * vy - y * vx);
  return [
    vx + w * tx + (y * tz - z * ty),
    vy + w * ty + (z * tx - x * tz),
    vz + w * tz + (x * ty - y * tx),
  ];
}

assert.deepEqual(makePlanarFrame(), { origin: [0, 0, 0], yawDeg: 0 });
assert.deepEqual(makePlanarFrame({ origin: [3, 4, 5], yawDeg: -30 }),
  { origin: [3, 4, 5], yawDeg: -30 });
assert.throws(() => makePlanarFrame({ origin: [0, 0] }), /frame\.origin/);
assert.throws(() => makePlanarFrame({ yawDeg: Infinity }), /frame\.yawDeg.*finite/);

// Frozen first fixture: the hall's local west mouth [0,0,4] reaches the
// requested world mouth [18,0,6] at +30 degrees without global-grid rounding.
const hallFrame = solvePlanarFrame([0, 0, 4], [18, 0, 6], 30);
near(hallFrame.origin, [16, 0, 2.5358983848622456], '30 degree hall origin');
near(transformPoint(hallFrame, [0, 0, 4]), [18, 0, 6], 'hall socket match');
const nestedFrame = composePlanarFrames(
  { origin: [10, 2, -4], yawDeg: 30 }, { origin: [2, 3, 1], yawDeg: 15 });
near(nestedFrame.origin, transformPoint({ origin: [10, 2, -4], yawDeg: 30 }, [2, 3, 1]));
assert.equal(nestedFrame.yawDeg, 45);

// The frozen convention is tested directly, including the sign of Z under a
// positive yaw.  This guards against importing a conventional +Y matrix whose
// off-diagonal signs differ from the site's contract.
for (const yawDeg of [15, 30, 45, -15, -30, -45]) {
  const radians = yawDeg * Math.PI / 180;
  const frame = { origin: [11, 2.5, -7], yawDeg };
  const point = [3.25, 4, -1.75];
  const expected = [
    11 + Math.cos(radians) * point[0] + Math.sin(radians) * point[2],
    6.5,
    -7 - Math.sin(radians) * point[0] + Math.cos(radians) * point[2],
  ];
  near(transformPoint(frame, point), expected, `${yawDeg} degree point`);
  near(inverseTransformPoint(frame, expected), point, `${yawDeg} degree inverse point`);
  near(inverseTransformVector(frame, transformVector(frame, point)), point,
    `${yawDeg} degree inverse vector`);
  near(inverseRotateXZ(rotateXZ([point[0], point[2]], yawDeg), yawDeg),
    [point[0], point[2]], `${yawDeg} degree inverse XZ vector`);
  near(inverseTransformPointXZ(frame, transformPointXZ(frame, [point[0], point[2]])),
    [point[0], point[2]], `${yawDeg} degree inverse XZ point`);
}

near(transformPoint({ origin: [4, 7, 9], yawDeg: 90 }, [1, 2, 0]), [4, 9, 8],
  'positive 90 yaw sends +X toward -Z');
near(transformVector({ origin: [400, 700, 900], yawDeg: 30 }, [0, -1, 0]), [0, -1, 0],
  'vectors ignore origin and preserve Y');

assert.deepEqual(directionToVector('E'), [1, 0, 0]);
assert.deepEqual(directionToVector('N'), [0, 0, 1]);
near(transformDirection({ yawDeg: 30 }, 'E'), [Math.sqrt(3) / 2, -0.5]);
near(transformDirection({ yawDeg: -45 }, [0, -0.8, 1]),
  [-Math.SQRT1_2, -0.8, Math.SQRT1_2]);
for (const yawDeg of [15, 30, 45, -15, -30, -45, 225])
  assert.ok(Math.abs(directionToYawDeg(yawDegToDirection(yawDeg)) - normalizeYawDeg(yawDeg)) < EPSILON);
assert.equal(transformYawDeg({ yawDeg: 30 }, 15), 45);
assert.equal(transformYawDeg({ yawDeg: -45 }, 15), -30);
assert.throws(() => directionToYawDeg([0, 4, 0]), /non-zero XZ/);

// xyzw quaternions must encode the same positive-yaw transform as points.
for (const yawDeg of [15, 30, 45, -30]) {
  const quaternion = yawDegToQuaternion(yawDeg);
  near(rotateByQuaternion([1, 0, 0], quaternion), yawDegToDirection(yawDeg),
    `${yawDeg} degree quaternion`);
  assert.ok(Math.abs(Math.hypot(...quaternion) - 1) < EPSILON);
}
quaternionNear(transformQuaternion({ yawDeg: 30 }, yawDegToQuaternion(15)),
  yawDegToQuaternion(45), 'frame/local yaw composition');
quaternionNear(multiplyQuaternions([0, 0, 0, -1], [0, 0, 0, 1]),
  [0, 0, 0, 1], 'quaternion canonical sign');

const matrixFrame = { origin: [10, 2, -3], yawDeg: 30 };
const frameMatrix = planarFrameMatrix(matrixFrame);
near([
  frameMatrix[0] * 3 + frameMatrix[1] * 4 + frameMatrix[2] * -2 + frameMatrix[3],
  frameMatrix[4] * 3 + frameMatrix[5] * 4 + frameMatrix[6] * -2 + frameMatrix[7],
  frameMatrix[8] * 3 + frameMatrix[9] * 4 + frameMatrix[10] * -2 + frameMatrix[11],
], transformPoint(matrixFrame, [3, 4, -2]), 'frame matrix follows point convention');
assert.deepEqual(frameMatrix.slice(12), [0, 0, 0, 1]);

const identityTransform = [
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 1, 0,
  0, 0, 0, 1,
];
near(multiplyTransforms(frameMatrix, identityTransform), frameMatrix,
  'right matrix identity');
assert.throws(() => multiplyTransforms(frameMatrix, [1, 0, 0]), /rightTransform/);

const localRootTransform = [
  0, 0, 1, 4,
  0, 2, 0, 5,
  -1, 0, 0, 6,
  0, 0, 0, 1,
];
const localRoot = { module: 'CastleWing', params: { wing: 2 }, transform: localRootTransform };
const worldRoot = transformRoot(matrixFrame, localRoot);
near(worldRoot.transform, multiplyTransforms(frameMatrix, localRootTransform));
near([worldRoot.transform[3], worldRoot.transform[7], worldRoot.transform[11]],
  transformPoint(matrixFrame, [4, 5, 6]), 'root local translation rotates before frame origin');
assert.equal(worldRoot.module, 'CastleWing');
assert.equal(worldRoot.params, localRoot.params);
assert.notEqual(worldRoot.transform, localRoot.transform);
assert.deepEqual(localRoot.transform, localRootTransform, 'root transform input is not mutated');

const localHullPoints = [0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1];
const localBoxCollider = {
  center: [0.25, 0, -0.5], halfExtents: [2, 1, 3],
  rotation: yawDegToQuaternion(15), friction: 0.8,
};
const localHullCollider = { points: localHullPoints, sensor: true };
const localEntity = {
  id: 'wing-collider',
  components: {
    LocalTransform: {
      translation: [4, 5, 6], rotation: yawDegToQuaternion(15), scale: [2, 3, 4],
    },
    RigidBody: { type: 'static' },
    BoxCollider: localBoxCollider,
    ConvexHullCollider: localHullCollider,
  },
};
const worldEntity = transformEntity(matrixFrame, localEntity);
near(worldEntity.components.LocalTransform.translation,
  transformPoint(matrixFrame, [4, 5, 6]));
quaternionNear(worldEntity.components.LocalTransform.rotation, yawDegToQuaternion(45));
assert.deepEqual(worldEntity.components.LocalTransform.scale, [2, 3, 4]);
assert.equal(worldEntity.components.BoxCollider, localBoxCollider,
  'BoxCollider stays in the entity local frame');
assert.equal(worldEntity.components.ConvexHullCollider, localHullCollider,
  'hull points stay in the entity local frame');
assert.equal(worldEntity.components.ConvexHullCollider.points, localHullPoints);
assert.equal(worldEntity.components.RigidBody, localEntity.components.RigidBody);
assert.notEqual(worldEntity.components, localEntity.components);
assert.notEqual(worldEntity.components.LocalTransform, localEntity.components.LocalTransform);
assert.deepEqual(localEntity.components.LocalTransform.translation, [4, 5, 6],
  'entity input is not mutated');
assert.throws(() => transformEntity(matrixFrame, { components: {} }), /LocalTransform/);

const bounds = { minX: 0, minY: 2, minZ: 0, maxX: 4, maxY: 8, maxZ: 2 };
const obb = transformAabbToObb({ origin: [10, 1, -3], yawDeg: 30 }, bounds);
near(obb.center, transformPoint({ origin: [10, 1, -3], yawDeg: 30 }, [2, 5, 1]));
assert.deepEqual(obb.halfExtents, [2, 3, 1]);
quaternionNear(obb.rotation, yawDegToQuaternion(30));
assert.deepEqual(bounds, { minX: 0, minY: 2, minZ: 0, maxX: 4, maxY: 8, maxZ: 2 },
  'AABB input remains local and unmodified');

const localBox = {
  center: [2, 5, 1], halfExtents: [2, 3, 1], rotation: yawDegToQuaternion(15),
  friction: 0.7,
};
const worldBox = transformBoxCollider({ origin: [10, 1, -3], yawDeg: 30 }, localBox);
near(worldBox.center, obb.center);
assert.deepEqual(worldBox.halfExtents, [2, 3, 1]);
quaternionNear(worldBox.rotation, yawDegToQuaternion(45));
assert.equal(worldBox.friction, 0.7);
assert.notEqual(worldBox.center, localBox.center);
assert.notEqual(worldBox.halfExtents, localBox.halfExtents);

const hullPoints = [
  0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2,
];
const hullFrame = { origin: [5, 6, 7], yawDeg: -45 };
const transformedHullPoints = transformHullPoints(hullFrame, hullPoints);
for (let index = 0; index < hullPoints.length; index += 3)
  near(transformedHullPoints.slice(index, index + 3),
    transformPoint(hullFrame, hullPoints.slice(index, index + 3)));
const worldHull = transformHullCollider(hullFrame, { points: hullPoints, sensor: true });
assert.deepEqual(worldHull.points, transformedHullPoints);
assert.equal(worldHull.sensor, true);
assert.deepEqual(hullPoints, [0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2]);
assert.throws(() => transformHullCollider(hullFrame, { points: [0, 0, 0] }), /4 to 32/);
assert.throws(() => transformHullPoints(hullFrame, [0, 1]), /flat xyz/);

const localSpot = {
  position: [2, 8, 1], direction: [0.6, -0.8, 0], color: [1, 0.5, 0.2],
  intensity: 90, inner: 12, outer: 25,
};
const spotFrame = { origin: [-4, 2, 9], yawDeg: 45 };
const worldSpot = transformSpotLight(spotFrame, localSpot);
near(worldSpot.position, transformPoint(spotFrame, localSpot.position));
near(worldSpot.direction, [Math.sqrt(0.18), -0.8, -Math.sqrt(0.18)]);
assert.ok(Math.abs(Math.hypot(...worldSpot.direction) - 1) < EPSILON,
  'rigid yaw preserves normalized spot direction');
assert.deepEqual(worldSpot.color, localSpot.color);
assert.equal(worldSpot.intensity, 90);
assert.notEqual(worldSpot.position, localSpot.position);
assert.notEqual(worldSpot.direction, localSpot.direction);

console.log('castle_frames_tests: ok');
