import {
  ClampToEdgeWrapping, DataTexture, LinearFilter, LinearMipmapLinearFilter, LinearSRGBColorSpace,
  MirroredRepeatWrapping, MeshStandardMaterial, NearestFilter, NearestMipmapNearestFilter, NoColorSpace,
  RGBAFormat, RepeatWrapping, SRGBColorSpace, UnsignedByteType,
  type Material, type Texture,
} from 'three';
import type {DiagnosticLog} from './errors.mjs';

/** Highest UV channel three.js materials can address (uv, uv1, uv2, uv3). */
export const MAX_UV_SET = 3;
export const DEFAULT_MAX_TEXTURE_SIZE = 4096;

/** A contract v1 resource descriptor. Counts are u64 hex strings on the wire;
 *  the JS SDK may hand over bigint or number before canonicalisation. */
export interface ResourceDescriptor {
  readonly id: string;
  readonly content: {readonly algorithm: string; readonly hex: string};
  readonly scalar_type: string;
  readonly components: number;
  readonly element_count: string | bigint | number;
  readonly byte_length: string | bigint | number;
}

export type ResourceResolver = (descriptor: ResourceDescriptor) => Uint8Array | undefined;

export interface MaterialOptions {
  readonly maxTextureSize?: number;
  /** Synchronous lookup for resource bytes the source did not carry. */
  readonly resolveResource?: ResourceResolver;
}

interface TextureValue {
  id: string; resource: string; color_space: string; wrap_u: string; wrap_v: string;
  min_filter: string; mag_filter: string; width: number; height: number; pixel_format: string;
}
interface MaterialValue {
  id: string;
  base_color: {factor: number[]; texture: string | null; uv_set: number};
  metallic: number; roughness: number; normal_scale: number; emissive: number[];
  metallic_roughness_texture: string | null; normal_texture: string | null; emissive_texture: string | null;
}

const WRAP: Record<string, number> = {repeat: RepeatWrapping, clamp: ClampToEdgeWrapping, mirror: MirroredRepeatWrapping};
const MIN_FILTER: Record<string, number> = {linear: LinearMipmapLinearFilter, nearest: NearestMipmapNearestFilter};
const MAG_FILTER: Record<string, number> = {linear: LinearFilter, nearest: NearestFilter};
const COLOR_SPACE: Record<string, string> = {srgb: SRGBColorSpace, linear: NoColorSpace};

const isObject = (value: unknown): value is Record<string, unknown> => typeof value === 'object' && value !== null && !Array.isArray(value);
const inUnit = (x: unknown) => typeof x === 'number' && Number.isFinite(x) && x >= 0 && x <= 1;

/** Accept a u64 as its canonical 16-hex-digit string, a bigint or a safe integer. */
export function u64(value: unknown): bigint | undefined {
  if (typeof value === 'bigint') return value >= 0n && value < 1n << 64n ? value : undefined;
  if (typeof value === 'number') return Number.isSafeInteger(value) && value >= 0 ? BigInt(value) : undefined;
  if (typeof value === 'string' && /^[0-9a-f]{16}$/.test(value)) return BigInt(`0x${value}`);
  return undefined;
}

export function hexBytes(hex: unknown): Uint8Array | undefined {
  if (typeof hex !== 'string' || hex.length % 2 !== 0 || !/^[0-9a-fA-F]*$/.test(hex)) return undefined;
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; ++i) out[i] = parseInt(hex.slice(2 * i, 2 * i + 2), 16);
  return out;
}

/** Bytes supplied alongside a document: `{id, bytes_hex}` (geometry wire) or `{id, bytes}`. */
export function collectResourceBytes(entries: unknown, log: DiagnosticLog): Map<string, Uint8Array> {
  const out = new Map<string, Uint8Array>();
  if (entries === undefined || entries === null) return out;
  if (!Array.isArray(entries)) log.fail('invalid_input', 'resources must be an array of {id, bytes_hex} or {id, bytes}');
  for (const entry of entries as unknown[]) {
    if (!isObject(entry) || typeof entry.id !== 'string') log.fail('invalid_input', 'resource bytes entry needs an id');
    const e = entry as Record<string, unknown>;
    const bytes = e.bytes instanceof Uint8Array ? e.bytes : hexBytes(e.bytes_hex);
    if (!bytes) log.fail('invalid_input', 'resource bytes must be a Uint8Array or an even-length hex string', {path: `resources/${e.id as string}`});
    if (out.has(e.id as string)) log.fail('invalid_input', 'duplicate resource bytes', {path: `resources/${e.id as string}`});
    out.set(e.id as string, bytes);
  }
  return out;
}

/** One three.js material per contract material and one texture upload per
 *  contract texture. A texture read through a different UV channel is a clone:
 *  clones share the uploaded image source, so they cost no extra texel memory,
 *  but each clone is still a texture object that must be disposed. */
export class MaterialLibrary {
  readonly materials = new Map<string, MeshStandardMaterial>();
  readonly #textures = new Map<string, Texture>();
  readonly #channelTextures = new Map<string, Texture>();
  readonly #specs = new Map<string, MaterialValue>();
  #disposed = false;

  constructor(asset: Record<string, unknown>, resourceBytes: ReadonlyMap<string, Uint8Array>, options: MaterialOptions, log: DiagnosticLog) {
    try {
      this.#build(asset, resourceBytes, options, log);
    } catch (error) {
      this.dispose();
      throw error;
    }
  }

  #build(asset: Record<string, unknown>, resourceBytes: ReadonlyMap<string, Uint8Array>, options: MaterialOptions, log: DiagnosticLog): void {
    const maxSize = options.maxTextureSize ?? DEFAULT_MAX_TEXTURE_SIZE;
    const resources = new Map<string, ResourceDescriptor>();
    for (const r of asset.resources as ResourceDescriptor[]) {
      if (!isObject(r) || typeof r.id !== 'string') log.fail('invalid_input', 'resource descriptor needs an id');
      if (resources.has(r.id)) log.fail('invalid_input', 'duplicate resource id', {path: `resources/${r.id}`});
      resources.set(r.id, r);
    }

    const textureValues = new Map<string, TextureValue>();
    for (const t of asset.textures as TextureValue[]) {
      const path = `textures/${isObject(t) ? String(t.id) : '?'}`;
      if (!isObject(t) || typeof t.id !== 'string') log.fail('invalid_input', 'texture needs an id', {path});
      if (textureValues.has(t.id)) log.fail('invalid_input', 'duplicate texture id', {path});
      if (t.pixel_format !== 'rgba8') log.fail('invalid_input', `pixel_format ${String(t.pixel_format)} is not rgba8`, {path});
      if (!(t.color_space in COLOR_SPACE)) log.fail('invalid_input', `unknown color_space ${String(t.color_space)}`, {path});
      if (!(t.wrap_u in WRAP) || !(t.wrap_v in WRAP)) log.fail('invalid_input', 'wrap must be repeat, clamp or mirror', {path});
      if (!(t.min_filter in MIN_FILTER) || !(t.mag_filter in MAG_FILTER)) log.fail('invalid_input', 'filters must be nearest or linear', {path});
      if (!Number.isInteger(t.width) || !Number.isInteger(t.height) || t.width < 1 || t.height < 1)
        log.fail('invalid_input', 'texture width/height must be positive integers', {path});
      if (t.width > maxSize || t.height > maxSize)
        log.fail('resource_limit', `texture ${t.width}x${t.height} exceeds the ${maxSize} texel limit`, {path});
      const descriptor = resources.get(t.resource);
      if (!descriptor) log.fail('missing_dependency', `texture references undeclared resource ${String(t.resource)}`, {path});
      const texels = BigInt(t.width) * BigInt(t.height);
      if (descriptor.scalar_type !== 'u8' || descriptor.components !== 4 || u64(descriptor.element_count) !== texels ||
          u64(descriptor.byte_length) !== texels * 4n)
        log.fail('invalid_input', `resource ${descriptor.id} must be u8 x4 with width*height elements for an rgba8 texture`, {path});
      // The content digest is the native loader's job (and the runtime's, which
      // received the bytes); the adapter checks shape so bad bytes cannot upload.
      const bytes = resourceBytes.get(descriptor.id) ?? options.resolveResource?.(descriptor);
      if (!bytes) log.fail('missing_dependency', `bytes for resource ${descriptor.id} were not supplied`, {path});
      if (BigInt(bytes.byteLength) !== texels * 4n)
        log.fail('invalid_input', `resource ${descriptor.id} has ${bytes.byteLength} bytes, expected ${texels * 4n}`, {path});
      textureValues.set(t.id, t);
      const texture = new DataTexture(new Uint8Array(bytes), t.width, t.height, RGBAFormat, UnsignedByteType);
      texture.name = t.id;
      // Row 0 is v = 0 and three.js samples DataTexture row 0 at v = 0 when flipY is false.
      texture.flipY = false;
      texture.colorSpace = COLOR_SPACE[t.color_space];
      texture.wrapS = WRAP[t.wrap_u];
      texture.wrapT = WRAP[t.wrap_v];
      texture.minFilter = MIN_FILTER[t.min_filter];
      texture.magFilter = MAG_FILTER[t.mag_filter];
      texture.generateMipmaps = true;  // v1: mips are derived by the consumer
      texture.needsUpdate = true;
      this.#textures.set(t.id, texture);
    }

    for (const m of asset.materials as MaterialValue[]) {
      const path = `materials/${isObject(m) ? String(m.id) : '?'}`;
      if (!isObject(m) || typeof m.id !== 'string') log.fail('invalid_input', 'material needs an id', {path});
      if (this.#specs.has(m.id)) log.fail('invalid_input', 'duplicate material id', {path});
      const base = m.base_color;
      // Same rule as the native validator: opaque baseline, alpha exactly 1.
      // Transparency arrives only as a declared extension, which is rejected above.
      if (!isObject(base) || !Array.isArray(base.factor) || base.factor.length !== 4 || !base.factor.every(inUnit) || base.factor[3] !== 1)
        log.fail('invalid_input', 'opaque base_color.factor must be RGB in [0,1] with alpha 1', {path});
      if (!inUnit(m.metallic) || !inUnit(m.roughness)) log.fail('invalid_input', 'metallic and roughness must be in [0,1]', {path});
      if (typeof m.normal_scale !== 'number' || !Number.isFinite(m.normal_scale) || m.normal_scale < 0)
        log.fail('invalid_input', 'normal_scale must be a finite number >= 0', {path});
      if (!Array.isArray(m.emissive) || m.emissive.length !== 3 || !m.emissive.every(x => typeof x === 'number' && Number.isFinite(x) && x >= 0))
        log.fail('invalid_input', 'emissive must be three finite numbers >= 0', {path});
      if (!Number.isInteger(base.uv_set) || base.uv_set < 0) log.fail('invalid_input', 'base_color.uv_set must be a non-negative integer', {path});
      if (base.uv_set > MAX_UV_SET)
        log.fail('unsupported_capability', `base_color.uv_set ${base.uv_set} exceeds the ${MAX_UV_SET + 1} UV channels three.js materials can sample`, {path});
      const textureOf = (field: string, ref: string | null, linear: boolean) => {
        if (ref === null) return null;
        const value = textureValues.get(ref);
        if (!value) log.fail('missing_dependency', `${field} references unknown texture ${String(ref)}`, {path});
        if (linear && value.color_space !== 'linear') log.fail('invalid_input', `${field} ${ref} carries data and must be linear`, {path});
        return ref;
      };
      const spec: MaterialValue = {...m, base_color: {...base}};
      textureOf('base_color.texture', base.texture, false);
      textureOf('metallic_roughness_texture', m.metallic_roughness_texture, true);
      textureOf('normal_texture', m.normal_texture, true);
      textureOf('emissive_texture', m.emissive_texture, false);
      this.#specs.set(m.id, spec);

      const material = new MeshStandardMaterial();
      material.name = m.id;
      // Factors are linear RGB; skip three.js's sRGB-input conversion.
      material.color.setRGB(base.factor[0], base.factor[1], base.factor[2], LinearSRGBColorSpace);
      material.metalness = m.metallic;
      material.roughness = m.roughness;
      material.emissive.setRGB(m.emissive[0], m.emissive[1], m.emissive[2], LinearSRGBColorSpace);
      // Only the base color names a UV set; every other texture uses set 0.
      if (base.texture) material.map = this.#channel(base.texture, base.uv_set);
      if (m.metallic_roughness_texture) {
        // Packed G = roughness, B = metallic: three.js reads exactly these channels
        // and multiplies them with the scalar factors, as the contract specifies.
        const mr = this.#channel(m.metallic_roughness_texture, 0);
        material.roughnessMap = mr;
        material.metalnessMap = mr;
      }
      if (m.normal_texture) {
        material.normalMap = this.#channel(m.normal_texture, 0);
        material.normalScale.set(m.normal_scale, m.normal_scale);
      }
      if (m.emissive_texture) material.emissiveMap = this.#channel(m.emissive_texture, 0);
      this.materials.set(m.id, material);
    }
  }

  #channel(textureId: string, channel: number): Texture {
    const base = this.#textures.get(textureId) as Texture;
    if (channel === 0) return base;
    const key = `${textureId}@${channel}`;
    let texture = this.#channelTextures.get(key);
    if (!texture) {
      texture = base.clone();
      texture.channel = channel;
      this.#channelTextures.set(key, texture);
    }
    return texture;
  }

  get(id: string): Material | undefined { return this.materials.get(id); }

  /** True when the material samples a normal map: geometry then needs tangents. */
  usesNormalMap(id: string): boolean { return Boolean(this.#specs.get(id)?.normal_texture); }

  /** UV channels the material samples (base color uses its declared set, others set 0). */
  uvSetsUsed(id: string): number[] {
    const m = this.#specs.get(id);
    if (!m) return [];
    const sets = new Set<number>();
    if (m.base_color.texture) sets.add(m.base_color.uv_set);
    if (m.metallic_roughness_texture || m.normal_texture || m.emissive_texture) sets.add(0);
    return [...sets].sort((a, b) => a - b);
  }

  get stats(): {materials: number; textures: number; textureObjects: number} {
    return {materials: this.materials.size, textures: this.#textures.size, textureObjects: this.#textures.size + this.#channelTextures.size};
  }

  get disposed(): boolean { return this.#disposed; }

  dispose(): void {
    if (this.#disposed) return;
    this.#disposed = true;
    for (const material of this.materials.values()) material.dispose();
    for (const texture of this.#channelTextures.values()) texture.dispose();
    for (const texture of this.#textures.values()) texture.dispose();
    this.materials.clear();
    this.#channelTextures.clear();
    this.#textures.clear();
  }
}
