export type Json = null | boolean | number | string | readonly Json[] | { readonly [key: string]: Json };
export type JsonObject = { readonly [key: string]: Json };
export type Seed = bigint | string;
export type Vec2 = readonly [number, number];
export type Vec3 = readonly [number, number, number];
export type Vec4 = readonly [number, number, number, number];
export type Matrix = readonly [number, number, number, number, number, number, number, number, number, number, number, number, number, number, number, number];
export type ProductType = 'mesh' | 'profile' | 'assembly';
export interface Capability { readonly name: string; readonly version: number }
export interface SourceLocation { readonly uri: string; readonly line: number; readonly column: number }
export interface ContentDigest { readonly algorithm: 'sha256'; readonly hex: string }
export interface SchemaVersion { readonly major: 1; readonly minor: 0 }
export interface SourceIdentity {
  readonly id: string; readonly uri: string; readonly revision: string;
  readonly content?: ContentDigest | null;
  readonly legacy_hashes?: readonly { readonly namespace: 'matter.source.v1' | 'matter.resolved.v1'; readonly value: string }[];
}
export interface Diagnostic {
  readonly severity: 'error'; readonly code: 'invalid_input' | 'unsupported_capability' | 'unsupported_version' | 'missing_dependency' | 'cycle' | 'resource_limit';
  readonly message: string; readonly node_id: string; readonly source: SourceLocation | null; readonly dependency_path: readonly string[];
}
export class AuthoringError extends Error { readonly diagnostic: Diagnostic; constructor(code: Diagnostic['code'], message: string, context?: { source?: SourceLocation; node_id?: string; dependency_path?: readonly string[] }); }
export const PROFILE: 'matter-exact-json-v1';
export const RNG_ALGORITHM: 'matter.splitmix64.v1';
export const DEFAULT_CAPABILITIES: readonly Capability[];
export const LEGACY_VOXELS: Capability;
export function identity(): Matrix;
export function translation(x: number, y: number, z: number): Matrix;
export function scale(x: number, y: number, z: number): Matrix;
export function rotation(axis: 'x' | 'y' | 'z', radians: number): Matrix;
export function multiply(parent: Matrix, local: Matrix): Matrix;
export function hex64(value: Seed): string;

export type ParameterSpec = { type: 'number' | 'integer'; default: number; minimum?: number; maximum?: number }
  | { type: 'boolean'; default: boolean } | { type: 'string'; default: string }
  | { type: 'enum'; default: string | number | boolean; values: readonly (string | number | boolean)[] };
export type ParameterValue<T extends ParameterSpec> = T extends { type: 'number' | 'integer' } ? number : T extends { type: 'boolean' } ? boolean : T extends { type: 'string' } ? string : T extends { values: readonly (infer V)[] } ? V : never;
export function parameters<const T extends Record<string, ParameterSpec>>(definitions: T, overrides?: Partial<{ [K in keyof T]: ParameterValue<T[K]> }>): Readonly<{ [K in keyof T]: ParameterValue<T[K]> }>;
export interface RngEngine { state: bigint; nextU64(): bigint }
export interface ContractServices {
  readonly canonicalProfile: typeof PROFILE; readonly rngAlgorithm: typeof RNG_ALGORITHM;
  canonicalJson(value: unknown): string; createRng(seed: bigint): RngEngine;
  readonly digestDocument?: (document: RecipeDocument) => Promise<ContentDigest>;
}
export function contractServices(helpers: { canonicalJson(value: unknown): string; Rng: new (seed: bigint) => RngEngine; contentDigest?: (document: RecipeDocument, digestBytes: (bytes: Uint8Array) => Promise<Uint8Array>) => Promise<ContentDigest> }, digestBytes?: (bytes: Uint8Array) => Promise<Uint8Array>): ContractServices;
export interface RngSnapshot { readonly algorithm: typeof RNG_ALGORITHM; readonly state: string; readonly draws: number }
export class SeededRng {
  constructor(seed: Seed, services: ContractServices);
  nextU64(): bigint; random(): number; nextUnit(): number; int(n: number): number; range(a: number, b: number): number;
  snapshot(): RngSnapshot; restore(snapshot: RngSnapshot): this; clone(): SeededRng;
}
export interface NodeRef<T extends ProductType = ProductType> { readonly port: string; readonly kind: 'node'; readonly reference: string; readonly type: T }
export interface Input { readonly port: string; readonly kind: 'node' | 'resource' | 'asset'; readonly reference: string; readonly type: ProductType }
export interface NodeOptions { readonly materials?: readonly string[]; readonly source?: SourceLocation; readonly transform?: Matrix; readonly operationVersion?: number }
export interface NodeSpec<T extends ProductType = ProductType> extends NodeOptions {
  readonly id: string; readonly operation: string; readonly outputType?: T; readonly inputs?: readonly Input[]; readonly parameters?: JsonObject;
}
export interface RecipeNode { readonly id: string; readonly operation: string; readonly operation_version: number; readonly output_type: ProductType; readonly inputs: readonly Input[]; readonly parameters: JsonObject; readonly materials: readonly string[]; readonly transform: Matrix }
export interface AssetDependency { readonly id: string; readonly content: ContentDigest; readonly schema_version: SchemaVersion }
export interface Resource { readonly id: string; readonly content: ContentDigest; readonly scalar_type: 'u8' | 'u32' | 'f32'; readonly components: number; readonly element_count: string; readonly byte_length: string }
export interface Texture { readonly id: string; readonly resource: string; readonly color_space: 'linear' | 'srgb'; readonly wrap_u: 'repeat' | 'clamp' | 'mirror'; readonly wrap_v: 'repeat' | 'clamp' | 'mirror'; readonly min_filter: 'nearest' | 'linear'; readonly mag_filter: 'nearest' | 'linear'; readonly width: number; readonly height: number; readonly pixel_format: 'rgba8' }
export interface Material {
  readonly id: string; readonly base_color: { readonly factor: Vec4; readonly texture: string | null; readonly uv_set: number };
  readonly metallic: number; readonly roughness: number; readonly normal_scale: number; readonly emissive: Vec3;
  readonly metallic_roughness_texture: string | null; readonly normal_texture: string | null; readonly emissive_texture: string | null;
}
export interface MaterialOptions { readonly baseColor?: Vec4; readonly metallic?: number; readonly roughness?: number; readonly normalScale?: number; readonly emissive?: Vec3; readonly baseColorTexture?: string; readonly uvSet?: number; readonly metallicRoughnessTexture?: string; readonly normalTexture?: string; readonly emissiveTexture?: string }
export interface Extension { readonly capability: Capability; readonly required: boolean; readonly payload: Json }
export interface RecipeDocument {
  readonly document_type: 'recipe'; readonly canonical_profile: typeof PROFILE; readonly schema_version: SchemaVersion;
  readonly required_capabilities: readonly Capability[]; readonly source: Required<SourceIdentity>; readonly parameters: JsonObject;
  readonly seed: string; readonly rng_algorithm: typeof RNG_ALGORITHM; readonly nodes: readonly RecipeNode[]; readonly outputs: readonly string[];
  readonly asset_dependencies: readonly AssetDependency[]; readonly resources: readonly Resource[]; readonly materials: readonly Material[];
  readonly textures: readonly Texture[]; readonly source_map: Readonly<Record<string, SourceLocation>>; readonly extensions: readonly Extension[]; readonly metadata: Json;
}
export interface Limits { readonly maxNodes?: number; readonly maxDependencies?: number; readonly maxDepth?: number; readonly maxValues?: number; readonly maxStringBytes?: number; readonly maxResourceBytes?: number; readonly maxVertices?: number; readonly maxIndices?: number }
export interface ValidationOptions { readonly supportedCapabilities?: readonly Capability[]; readonly limits?: Limits }
export interface BuilderOptions extends ValidationOptions { readonly source: SourceIdentity; readonly seed: Seed; readonly parameters?: JsonObject; readonly requiredCapabilities?: readonly Capability[]; readonly services?: ContractServices }
export function nodeInput<T extends ProductType = 'mesh'>(reference: string, type?: T, port?: string): NodeRef<T>;
export function validateRecipe(document: unknown, options?: ValidationOptions): RecipeDocument;
export class RecipeBuilder {
  constructor(options: BuilderOptions);
  require(capability: Capability): this;
  at<T>(source: SourceLocation, callback: (builder: this) => T): T;
  node<T extends ProductType = 'mesh'>(spec: NodeSpec<T>): NodeRef<T>;
  box(id: string, size: Vec3, options?: NodeOptions): NodeRef<'mesh'>;
  sphere(id: string, radius: number, segments?: number, options?: NodeOptions): NodeRef<'mesh'>;
  polygon(id: string, points: readonly Vec2[], options?: NodeOptions): NodeRef<'profile'>;
  extrude(id: string, profile: NodeRef<'profile'>, depth: number, options?: NodeOptions): NodeRef<'mesh'>;
  csg(id: string, operator: 'union' | 'subtract' | 'intersect', operands: readonly (NodeRef<'mesh'> | string)[], options?: NodeOptions): NodeRef<'mesh'>;
  place<T extends 'mesh' | 'assembly'>(id: string, reference: NodeRef<T>, transform: Matrix, options?: NodeOptions): NodeRef<T>;
  place(id: string, reference: string, transform: Matrix, options?: NodeOptions): NodeRef<'mesh'>;
  assembly(id: string, dependencyId: string, options?: NodeOptions): NodeRef<'assembly'>;
  mesh(id: string, buffers: Readonly<Partial<Record<'positions' | 'indices' | 'normals' | 'tangents' | 'uv0', string>>>, material: string, options?: NodeOptions): NodeRef<'mesh'>;
  repeat<T extends 'mesh' | 'assembly'>(prefix: string, count: number, reference: NodeRef<T>, transformAt: (index: number) => Matrix): readonly NodeRef<T>[];
  dependency(value: AssetDependency): string;
  resource(value: Omit<Resource, 'element_count' | 'byte_length'> & { element_count: Seed; byte_length: Seed }): string;
  texture(value: Texture): string; material(id: string, options?: MaterialOptions): string;
  extension(value: Extension): this; metadata(value: Json): this; output(...references: readonly (NodeRef | string)[]): this;
  compose(prefix: string, document: RecipeDocument): readonly NodeRef[];
  rng(): SeededRng; build(): RecipeDocument; canonical(): string; digest(): Promise<ContentDigest>;
}
export interface LegacyOptions {
  readonly prefix?: string; readonly sourceLocation?: () => SourceLocation;
  readonly materialIds?: Readonly<Record<number, string>>;
  readonly resolveChild?: (request: { readonly module: string; readonly parameters: JsonObject }) => AssetDependency;
}
export class LegacyPart {
  constructor(builder: RecipeBuilder, options?: LegacyOptions);
  invoke(verb: string, ...args: unknown[]): unknown;
  pushMatrix(): void; popMatrix(): void; translate(x: number, y: number, z: number): void;
  rotateX(radians: number): void; rotateY(radians: number): void; rotateZ(radians: number): void;
  scale(x: number, y: number, z: number): void; applyMatrix(matrix: Matrix): void; fill(material: string | number): void;
  box(center: Vec3, halfExtents: Vec3): NodeRef<'mesh'>; sphere(center: Vec3, radius: number): NodeRef<'mesh'>;
  beginVoxels(spacing?: number): void; endVoxels(): NodeRef<'mesh'>; smoothing(metres: number): void;
  union(): void; difference(): void; intersection(): void; placeChild(module: string, parameters?: JsonObject, options?: Record<string, never>): NodeRef<'assembly'>;
  finish(): RecipeDocument;
}
export function legacyPart(builder: RecipeBuilder, options?: LegacyOptions): LegacyPart;
