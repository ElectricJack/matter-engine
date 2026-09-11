// Exact rigid planar frames for composing independently authored castle wings.
//
// Coordinate convention (metres, degrees): positive yaw is the engine's +Y
// rotation and therefore maps local coordinates as
//   x' = cos(yaw) * x + sin(yaw) * z
//   z' = -sin(yaw) * x + cos(yaw) * z
// Points receive the frame origin; vectors and directions never do.  Nothing
// in this module snaps or rounds rotated coordinates back to the plan grid.

const DEG_TO_RAD = Math.PI / 180;
const RAD_TO_DEG = 180 / Math.PI;

export const CARDINAL_XZ = Object.freeze({
  E: Object.freeze([1, 0]),
  N: Object.freeze([0, 1]),
  W: Object.freeze([-1, 0]),
  S: Object.freeze([0, -1]),
});

function fail(path, message) {
  throw new TypeError(`castle frame ${path}: ${message}`);
}

function finite(value, path) {
  if (typeof value !== 'number' || !Number.isFinite(value))
    fail(path, 'must be finite');
  return value;
}

function array(value, length, path) {
  if (!Array.isArray(value) || value.length !== length)
    fail(path, `must be a ${length}-element array`);
  return value.map((component, index) => finite(component, `${path}[${index}]`));
}

function quaternion(value, path) {
  const q = array(value, 4, path);
  const magnitude = Math.hypot(...q);
  if (!(magnitude > 0) || !Number.isFinite(magnitude))
    fail(path, 'must have non-zero finite length');
  const result = q.map(component => component / magnitude);
  // q and -q are the same rotation.  Prefer a non-negative scalar component
  // so JSON output is stable when composed from equivalent inputs.
  return result[3] < 0 ? result.map(component => -component) : result;
}

function frame(value) {
  if (value === undefined) return { origin: [0, 0, 0], yawDeg: 0 };
  if (!value || typeof value !== 'object' || Array.isArray(value))
    fail('frame', 'must be an object');
  return {
    origin: array(value.origin ?? [0, 0, 0], 3, 'frame.origin'),
    yawDeg: finite(value.yawDeg ?? 0, 'frame.yawDeg'),
  };
}

function yawTerms(yawDeg) {
  const radians = finite(yawDeg, 'yawDeg') * DEG_TO_RAD;
  return [Math.cos(radians), Math.sin(radians)];
}

function directionXZ(value, path) {
  if (typeof value === 'string') {
    const result = CARDINAL_XZ[value];
    if (!result) fail(path, 'must be E, N, W, S, [x,z], or [x,y,z]');
    return [...result];
  }
  if (!Array.isArray(value) || (value.length !== 2 && value.length !== 3))
    fail(path, 'must be E, N, W, S, [x,z], or [x,y,z]');
  return value.length === 2
    ? array(value, 2, path)
    : [finite(value[0], `${path}[0]`), finite(value[2], `${path}[2]`)];
}

export function makePlanarFrame(value = {}) {
  return frame(value);
}

export function composePlanarFrames(parentFrame, localFrame) {
  const parent = frame(parentFrame);
  const local = frame(localFrame);
  return {
    origin: transformPoint(parent, local.origin),
    yawDeg: normalizeYawDeg(parent.yawDeg + local.yawDeg),
  };
}

// Solve the translation after yaw is known: R * localPoint + origin = worldPoint.
// This is the socket-placement equation used by compileSite().
export function solvePlanarFrame(localPoint, worldPoint, yawDeg) {
  const local = array(localPoint, 3, 'localPoint');
  const world = array(worldPoint, 3, 'worldPoint');
  const rotated = transformVector({ yawDeg }, local);
  return {
    origin: world.map((component, index) => component - rotated[index]),
    yawDeg: finite(yawDeg, 'yawDeg'),
  };
}

// Canonical signed representation used only for angle/quaternion publication;
// point and vector transforms use the caller's angle directly.
export function normalizeYawDeg(value) {
  const yawDeg = finite(value, 'yawDeg');
  const wrapped = ((yawDeg + 180) % 360 + 360) % 360 - 180;
  return Object.is(wrapped, -0) ? 0 : wrapped;
}

export function rotateXZ(value, yawDeg) {
  const [x, z] = array(value, 2, 'vectorXZ');
  const [cosine, sine] = yawTerms(yawDeg);
  return [cosine * x + sine * z, -sine * x + cosine * z];
}

export function inverseRotateXZ(value, yawDeg) {
  const [x, z] = array(value, 2, 'vectorXZ');
  const [cosine, sine] = yawTerms(yawDeg);
  return [cosine * x - sine * z, sine * x + cosine * z];
}

export function transformPointXZ(frameValue, point) {
  const f = frame(frameValue);
  const [x, z] = rotateXZ(point, f.yawDeg);
  return [x + f.origin[0], z + f.origin[2]];
}

export function inverseTransformPointXZ(frameValue, point) {
  const f = frame(frameValue);
  const [x, z] = array(point, 2, 'pointXZ');
  return inverseRotateXZ([x - f.origin[0], z - f.origin[2]], f.yawDeg);
}

export function transformPoint(frameValue, point) {
  const f = frame(frameValue);
  const [x, y, z] = array(point, 3, 'point');
  const [worldX, worldZ] = rotateXZ([x, z], f.yawDeg);
  return [worldX + f.origin[0], y + f.origin[1], worldZ + f.origin[2]];
}

export function inverseTransformPoint(frameValue, point) {
  const f = frame(frameValue);
  const [x, y, z] = array(point, 3, 'point');
  const [localX, localZ] = inverseRotateXZ(
    [x - f.origin[0], z - f.origin[2]], f.yawDeg);
  return [localX, y - f.origin[1], localZ];
}

export function transformVector(frameValue, vector) {
  const f = frame(frameValue);
  const [x, y, z] = array(vector, 3, 'vector');
  const [worldX, worldZ] = rotateXZ([x, z], f.yawDeg);
  return [worldX, y, worldZ];
}

export function inverseTransformVector(frameValue, vector) {
  const f = frame(frameValue);
  const [x, y, z] = array(vector, 3, 'vector');
  const [localX, localZ] = inverseRotateXZ([x, z], f.yawDeg);
  return [localX, y, localZ];
}

export function directionToVector(value) {
  const [x, z] = directionXZ(value, 'direction');
  return [x, 0, z];
}

// A yaw heading is the image of local +X under the frozen frame convention.
export function yawDegToDirection(yawDeg) {
  const [x, z] = rotateXZ([1, 0], yawDeg);
  return [x, 0, z];
}

export function directionToYawDeg(value) {
  const [x, z] = directionXZ(value, 'direction');
  if (Math.hypot(x, z) <= Number.EPSILON)
    fail('direction', 'must have a non-zero XZ projection');
  return normalizeYawDeg(Math.atan2(-z, x) * RAD_TO_DEG);
}

export function transformDirection(frameValue, value) {
  const f = frame(frameValue);
  if (typeof value === 'string' || (Array.isArray(value) && value.length === 2)) {
    const [x, z] = directionXZ(value, 'direction');
    return rotateXZ([x, z], f.yawDeg);
  }
  return transformVector(f, array(value, 3, 'direction'));
}

export function transformYawDeg(frameValue, localYawDeg) {
  const f = frame(frameValue);
  return normalizeYawDeg(f.yawDeg + finite(localYawDeg, 'localYawDeg'));
}

export function yawDegToQuaternion(yawDeg) {
  const radians = normalizeYawDeg(yawDeg) * DEG_TO_RAD;
  return [0, Math.sin(radians * 0.5), 0, Math.cos(radians * 0.5)];
}

// Row-major matrix for column-vector algebra, matching World root transforms
// and the engine's LocalTransform expansion.  Translation occupies 3/7/11.
export function planarFrameMatrix(frameValue) {
  const f = frame(frameValue);
  const [cosine, sine] = yawTerms(f.yawDeg);
  return [
    cosine, 0, sine, f.origin[0],
    0, 1, 0, f.origin[1],
    -sine, 0, cosine, f.origin[2],
    0, 0, 0, 1,
  ];
}

export function multiplyTransforms(left, right) {
  const a = array(left, 16, 'leftTransform');
  const b = array(right, 16, 'rightTransform');
  const result = new Array(16).fill(0);
  for (let row = 0; row < 4; ++row) {
    for (let column = 0; column < 4; ++column) {
      for (let inner = 0; inner < 4; ++inner)
        result[row * 4 + column] += a[row * 4 + inner] * b[inner * 4 + column];
    }
  }
  return result;
}

export function multiplyQuaternions(left, right) {
  const a = quaternion(left, 'leftQuaternion');
  const b = quaternion(right, 'rightQuaternion');
  return quaternion([
    a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
    a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
  ], 'quaternion product');
}

// Pre-multiplication means the local rotation happens first, then the wing's
// world-space yaw, matching T(frame) * R(frame) * local geometry.
export function transformQuaternion(frameValue, localQuaternion = [0, 0, 0, 1]) {
  const f = frame(frameValue);
  return multiplyQuaternions(yawDegToQuaternion(f.yawDeg), localQuaternion);
}

// World roots already carry a complete local row-major transform.  The wing
// frame is a parent transform, so it is pre-multiplied rather than patched into
// the translation slots (which would lose rotation of the local translation).
export function transformRoot(frameValue, root) {
  if (!root || typeof root !== 'object' || Array.isArray(root))
    fail('root', 'must be an object');
  return {
    ...root,
    transform: multiplyTransforms(planarFrameMatrix(frameValue), root.transform),
  };
}

// ECS collider data is entity-local.  Applying the wing frame to both the
// LocalTransform and BoxCollider/ConvexHullCollider would rotate it twice, so
// only the entity pose is composed here.  Record-level world-space collider
// conversion remains available separately through transformBoxCollider() and
// transformHullCollider().
export function transformEntity(frameValue, entity) {
  if (!entity || typeof entity !== 'object' || Array.isArray(entity))
    fail('entity', 'must be an object');
  if (!entity.components || typeof entity.components !== 'object' ||
      Array.isArray(entity.components))
    fail('entity.components', 'must be an object');
  const local = entity.components.LocalTransform;
  if (!local || typeof local !== 'object' || Array.isArray(local))
    fail('entity.components.LocalTransform', 'must be an object');
  const translation = array(local.translation ?? [0, 0, 0], 3,
    'entity.components.LocalTransform.translation');
  const rotation = local.rotation ?? [0, 0, 0, 1];
  if (local.scale !== undefined)
    array(local.scale, 3, 'entity.components.LocalTransform.scale');
  return {
    ...entity,
    components: {
      ...entity.components,
      LocalTransform: {
        ...local,
        translation: transformPoint(frameValue, translation),
        rotation: transformQuaternion(frameValue, rotation),
      },
    },
  };
}

export function transformAabbToObb(frameValue, bounds) {
  if (!bounds || typeof bounds !== 'object' || Array.isArray(bounds))
    fail('bounds', 'must be an object');
  const minX = finite(bounds.minX, 'bounds.minX');
  const minY = finite(bounds.minY, 'bounds.minY');
  const minZ = finite(bounds.minZ, 'bounds.minZ');
  const maxX = finite(bounds.maxX, 'bounds.maxX');
  const maxY = finite(bounds.maxY, 'bounds.maxY');
  const maxZ = finite(bounds.maxZ, 'bounds.maxZ');
  if (maxX < minX || maxY < minY || maxZ < minZ)
    fail('bounds', 'maximums must not be less than minimums');
  return {
    center: transformPoint(frameValue,
      [(minX + maxX) * 0.5, (minY + maxY) * 0.5, (minZ + maxZ) * 0.5]),
    halfExtents: [(maxX - minX) * 0.5, (maxY - minY) * 0.5, (maxZ - minZ) * 0.5],
    rotation: yawDegToQuaternion(frame(frameValue).yawDeg),
  };
}

// BoxCollider is already an OBB: rotate its center and compose its local xyzw
// rotation while retaining dimensions and material/filter properties.
export function transformBoxCollider(frameValue, collider) {
  if (!collider || typeof collider !== 'object' || Array.isArray(collider))
    fail('boxCollider', 'must be an object');
  const center = array(collider.center ?? [0, 0, 0], 3, 'boxCollider.center');
  const halfExtents = array(collider.halfExtents, 3, 'boxCollider.halfExtents');
  if (halfExtents.some(component => component < 0))
    fail('boxCollider.halfExtents', 'must be non-negative');
  return {
    ...collider,
    center: transformPoint(frameValue, center),
    halfExtents,
    rotation: transformQuaternion(frameValue, collider.rotation ?? [0, 0, 0, 1]),
  };
}

export function transformHullPoints(frameValue, points) {
  if (!Array.isArray(points) || points.length % 3 !== 0)
    fail('hullPoints', 'must be a flat xyz array');
  const transformed = [];
  for (let index = 0; index < points.length; index += 3)
    transformed.push(...transformPoint(frameValue, points.slice(index, index + 3)));
  return transformed;
}

export function transformHullCollider(frameValue, collider) {
  if (!collider || typeof collider !== 'object' || Array.isArray(collider))
    fail('hullCollider', 'must be an object');
  if (!Array.isArray(collider.points))
    fail('hullCollider.points', 'must be a flat xyz array');
  const pointCount = collider.points.length / 3;
  if (!Number.isInteger(pointCount) || pointCount < 4 || pointCount > 32)
    fail('hullCollider.points', 'must contain 4 to 32 xyz points');
  return { ...collider, points: transformHullPoints(frameValue, collider.points) };
}

export function transformSpotLight(frameValue, light) {
  if (!light || typeof light !== 'object' || Array.isArray(light))
    fail('spotLight', 'must be an object');
  return {
    ...light,
    position: transformPoint(frameValue, light.position),
    direction: transformVector(frameValue, light.direction),
  };
}
