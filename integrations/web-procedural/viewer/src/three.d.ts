// Minimal declarations for the vendored three.js 0.186.1 build
// (vendor/three/three.module.js). Only the surface this adapter and demo use;
// pages map the bare 'three' specifier to the vendored module with an import map.
declare module 'three' {
  export const REVISION: string;
  export const SRGBColorSpace: string;
  export const LinearSRGBColorSpace: string;
  export const NoColorSpace: string;
  export const RepeatWrapping: number;
  export const ClampToEdgeWrapping: number;
  export const MirroredRepeatWrapping: number;
  export const NearestFilter: number;
  export const LinearFilter: number;
  export const NearestMipmapNearestFilter: number;
  export const LinearMipmapLinearFilter: number;
  export const RGBAFormat: number;
  export const UnsignedByteType: number;
  export const FrontSide: number;
  export const NoToneMapping: number;
  export const NeutralToneMapping: number;

  export interface BaseEvent { type: string; target?: unknown }
  export class EventDispatcher {
    addEventListener(type: string, listener: (event: BaseEvent) => void): void;
    removeEventListener(type: string, listener: (event: BaseEvent) => void): void;
  }
  export class Matrix4 {
    elements: number[];
    set(n11: number, n12: number, n13: number, n14: number, n21: number, n22: number, n23: number, n24: number,
      n31: number, n32: number, n33: number, n34: number, n41: number, n42: number, n43: number, n44: number): this;
    identity(): this;
    determinant(): number;
  }
  export class Vector2 { x: number; y: number; constructor(x?: number, y?: number); set(x: number, y: number): this }
  export class Vector3 {
    x: number; y: number; z: number;
    constructor(x?: number, y?: number, z?: number);
    set(x: number, y: number, z: number): this;
    copy(v: Vector3): this;
    clone(): Vector3;
    length(): number;
    applyMatrix4(m: Matrix4): this;
    setFromMatrixPosition(m: Matrix4): this;
    project(camera: Camera): this;
  }
  export class Color {
    r: number; g: number; b: number;
    constructor(color?: number | string);
    setRGB(r: number, g: number, b: number, colorSpace?: string): this;
    set(color: number | string): this;
  }
  export class Box3 {
    min: Vector3; max: Vector3;
    constructor(min?: Vector3, max?: Vector3);
    setFromObject(object: Object3D): this;
    isEmpty(): boolean;
    getCenter(target: Vector3): Vector3;
    getSize(target: Vector3): Vector3;
  }
  export class Object3D extends EventDispatcher {
    name: string;
    parent: Object3D | null;
    children: Object3D[];
    position: Vector3;
    scale: Vector3;
    matrix: Matrix4;
    matrixWorld: Matrix4;
    matrixAutoUpdate: boolean;
    matrixWorldNeedsUpdate: boolean;
    visible: boolean;
    userData: Record<string, unknown>;
    add(...objects: Object3D[]): this;
    remove(...objects: Object3D[]): this;
    removeFromParent(): this;
    updateMatrixWorld(force?: boolean): void;
    lookAt(x: number, y: number, z: number): void;
    traverse(callback: (object: Object3D) => void): void;
    clear(): this;
  }
  export class Group extends Object3D {}
  export class Scene extends Object3D { background: Color | null }
  export class Camera extends Object3D {}
  export class PerspectiveCamera extends Camera {
    fov: number; aspect: number; near: number; far: number;
    constructor(fov?: number, aspect?: number, near?: number, far?: number);
    updateProjectionMatrix(): void;
  }
  export class Light extends Object3D { color: Color; intensity: number }
  export class AmbientLight extends Light { constructor(color?: number, intensity?: number) }
  export class HemisphereLight extends Light { constructor(sky?: number, ground?: number, intensity?: number) }
  export class DirectionalLight extends Light { constructor(color?: number, intensity?: number) }

  export type TypedArray = Float32Array | Uint32Array | Uint16Array | Uint8Array;
  export class BufferAttribute {
    array: TypedArray; itemSize: number; count: number; needsUpdate: boolean;
    constructor(array: TypedArray, itemSize: number, normalized?: boolean);
  }
  export class InstancedBufferAttribute extends BufferAttribute {}
  export interface GeometryGroup { start: number; count: number; materialIndex?: number }
  export class BufferGeometry extends EventDispatcher {
    name: string;
    attributes: Record<string, BufferAttribute>;
    index: BufferAttribute | null;
    groups: GeometryGroup[];
    userData: Record<string, unknown>;
    setAttribute(name: string, attribute: BufferAttribute): this;
    getAttribute(name: string): BufferAttribute | undefined;
    setIndex(index: BufferAttribute): this;
    addGroup(start: number, count: number, materialIndex?: number): void;
    computeVertexNormals(): void;
    computeBoundingSphere(): void;
    dispose(): void;
  }
  export class Texture extends EventDispatcher {
    name: string; colorSpace: string; wrapS: number; wrapT: number; magFilter: number; minFilter: number;
    generateMipmaps: boolean; flipY: boolean; channel: number; needsUpdate: boolean;
    userData: Record<string, unknown>;
    /** Clones share `source` (one GPU upload); each clone must still be disposed. */
    clone(): this;
    dispose(): void;
  }
  export class DataTexture extends Texture {
    constructor(data: Uint8Array, width: number, height: number, format?: number, type?: number);
  }
  export class Material extends EventDispatcher {
    name: string; side: number; transparent: boolean; needsUpdate: boolean;
    userData: Record<string, unknown>;
    dispose(): void;
  }
  export class MeshStandardMaterial extends Material {
    color: Color; emissive: Color; emissiveIntensity: number; metalness: number; roughness: number;
    map: Texture | null; roughnessMap: Texture | null; metalnessMap: Texture | null;
    normalMap: Texture | null; normalScale: Vector2; emissiveMap: Texture | null;
    constructor(parameters?: Record<string, unknown>);
  }
  export class Mesh extends Object3D {
    geometry: BufferGeometry; material: Material | Material[]; frustumCulled: boolean;
    constructor(geometry?: BufferGeometry, material?: Material | Material[]);
  }
  export class InstancedMesh extends Mesh {
    count: number; instanceMatrix: InstancedBufferAttribute;
    constructor(geometry: BufferGeometry, material: Material | Material[], count: number);
    setMatrixAt(index: number, matrix: Matrix4): void;
    computeBoundingSphere(): void;
    dispose(): this;
  }
  export interface WebGLInfo {
    autoReset: boolean;
    render: {calls: number; triangles: number; frame: number};
    memory: {geometries: number; textures: number};
    programs: unknown[] | null;
    reset(): void;
  }
  export interface WebGLRendererParameters {
    canvas?: HTMLCanvasElement; context?: WebGL2RenderingContext; antialias?: boolean;
    preserveDrawingBuffer?: boolean; alpha?: boolean; powerPreference?: string;
  }
  export class WebGLRenderer {
    domElement: HTMLCanvasElement; info: WebGLInfo; outputColorSpace: string; toneMapping: number;
    capabilities: {maxTextureSize: number};
    constructor(parameters?: WebGLRendererParameters);
    setSize(width: number, height: number, updateStyle?: boolean): void;
    setPixelRatio(ratio: number): void;
    setClearColor(color: number, alpha?: number): void;
    render(scene: Object3D, camera: Camera): void;
    getContext(): WebGL2RenderingContext;
    dispose(): void;
  }
}
