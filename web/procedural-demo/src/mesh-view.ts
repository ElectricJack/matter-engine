import { BufferAttribute, BufferGeometry, Color, Group, Mesh, MeshNormalMaterial, MeshStandardMaterial } from 'three';
import type { EvaluationResult } from '../../matter-procedural-wasm/index.d.ts';

export interface MeshView {
  readonly root: Group;
  readonly triangles: number;
  setDisplay(wireframe: boolean, normals: boolean): void;
  dispose(): void;
}

/** Package v1 adapter: prototypes share geometry; only instances become draw objects. */
export function createMeshView(result: EvaluationResult): MeshView {
  const root = new Group();
  const geometries = new Map<string, BufferGeometry>();
  const materials = result.materials.map(material => {
    const [r, g, b, opacity] = material.base_color.factor;
    return new MeshStandardMaterial({
      color: new Color(r, g, b), opacity, transparent: opacity < 1,
      roughness: material.roughness, metalness: material.metallic,
      emissive: new Color(...material.emissive),
    });
  });
  const normalMaterial = new MeshNormalMaterial();
  const meshes = new Map(result.meshes.map(mesh => [mesh.id, mesh]));
  let triangles = 0, disposed = false;
  const release = () => {
    geometries.forEach(geometry => geometry.dispose());
    materials.forEach(material => material.dispose()); normalMaterial.dispose();
  };
  try {
    for (const instance of result.instances) {
      const buffers = meshes.get(instance.meshId);
      if (!buffers) throw new Error(`Missing prototype ${instance.meshId} for ${instance.id}`);
      let geometry = geometries.get(buffers.id);
      if (!geometry) {
        geometry = new BufferGeometry(); geometries.set(buffers.id, geometry);
        geometry.setAttribute('position', new BufferAttribute(buffers.positions, 3));
        geometry.setIndex(new BufferAttribute(buffers.indices, 1));
        if (buffers.normals.length) geometry.setAttribute('normal', new BufferAttribute(buffers.normals, 3));
        else geometry.computeVertexNormals();
        if (buffers.uvs.length) geometry.setAttribute('uv', new BufferAttribute(buffers.uvs, 2));
        for (const group of buffers.groups) {
          if (!materials[group.materialIndex]) throw new Error(`Unknown material ${group.materialIndex}`);
          geometry.addGroup(group.start, group.count, group.materialIndex);
        }
        // Preserve provenance for inspection/picking without turning per-face IDs into vertices.
        geometry.userData = { materialIds: buffers.materialIds, partIds: buffers.partIds, parts: result.parts };
        geometry.computeBoundingSphere();
      }
      const mesh = new Mesh(geometry, materials);
      mesh.name = instance.id; mesh.matrixAutoUpdate = false;
      // Matrix4.set accepts row-major; fromArray would transpose this contract matrix.
      mesh.matrix.set(...instance.transform);
      mesh.userData = { partId: instance.partId, meshId: instance.meshId, surfaceMaterials: materials };
      root.add(mesh); triangles += buffers.indices.length / 3;
    }
    root.updateMatrixWorld(true);
  } catch (error) { release(); throw error; }
  return {
    root, triangles,
    setDisplay(wireframe, normals) {
      normalMaterial.wireframe = wireframe;
      materials.forEach(material => { material.wireframe = wireframe; });
      root.children.forEach(object => { (object as Mesh).material = normals ? normalMaterial : materials; });
    },
    dispose() {
      if (disposed) return;
      disposed = true; root.removeFromParent(); root.clear(); release();
    },
  };
}
