import { brickWallLayout } from 'shared-lib/brick_wall_layout';
import { makeSurfaceFace, emitSurfaceShell } from 'shared-lib/castle_surface_shells';

// Inspection geometry consumes the same placements intended for VT stamps.
// High-resolution source meshes here are a review representation, not the
// final low-poly runtime wall. No wall-size parameter enters a source variant.
export function brickWallGeometryPlan(p = {}) {
  const layout = brickWallLayout(p);
  const representation = p.representation ?? 'source';
  if (!['source', 'surface'].includes(representation)) throw new RangeError('unknown brick representation');
  // Native Part parameters transport scalar material handles. Keep the array
  // form for direct JS callers, but use matBrick0..7 through authored roots.
  const materials = p.brickMaterials ?? Array.from({length:8},(_,i)=>p['matBrick'+i] ?? 8);
  const mortarMaterial = p.mortarMaterial ?? 9;
  if (!Array.isArray(materials) || materials.length !== 8 ||
      ![...materials,mortarMaterial].every(n => Number.isInteger(n) && n>=0))
    throw new TypeError('wall needs eight brick material handles and one mortar handle');
  const voxelM = p.voxelM ?? .003;
  if (!Number.isFinite(voxelM) || voxelM<.001 || voxelM>.004) throw new RangeError('preview voxel pitch must be 1–4 mm');
  const b = layout.brick;
  if (![b.lengthM,b.heightM,b.depthM].every(n => n>=.06 && n<=.5))
    throw new RangeError('ClayBrickSource dimensions must be 0.06–0.5 m');
  const source = variant => ({ module: representation === 'surface' ? 'ClayBrickSurface' : 'ClayBrickSource', params: {
    seed: variant, length: b.lengthM, height: b.heightM, depth: b.depthM,
    voxelM, material: materials[variant], maxVertices: 500000,
    ...(representation === 'surface' ? {pixelM:p.pixelM ?? .001} : {}) } });
  const children = layout.placements.map(p => ({ ...p, ...source(p.variant) }));
  const requires = [...new Set(layout.placements.map(p => p.variant))].sort().map(source);
  const faces = [];
  for (const gap of layout.mortar) {
    const a=gap.min, z=gap.max;
    for (let axis=0;axis<3;++axis) for (const sign of [-1,1]) {
      const i=(axis+1)%3, j=(axis+2)%3, n=[0,0,0]; n[axis]=sign;
      const points=[];
      for (const [u,v] of [[0,0],[1,0],[1,1],[0,1]]) {
        const p=[...a]; p[axis]=sign<0?a[axis]:z[axis]; p[i]=u?z[i]:a[i]; p[j]=v?z[j]:a[j]; points.push(p);
      }
      faces.push(makeSurfaceFace(points,n,`${gap.id}/${axis}/${sign}`,'body'));
    }
  }
  return { layout, children, requires, mortar: { faces }, mortarMaterial };
}
export function emitBrickWallGeometry(part, plan) {
  emitSurfaceShell(part,plan.mortar,{body:plan.mortarMaterial});
  for (const child of plan.children) {
    part.pushMatrix(); part.applyMatrix(child.matrix);
    part.placeChild(child.module,child.params,{instanced:true,inlineBelowPx:.25});
    part.popMatrix();
  }
}
