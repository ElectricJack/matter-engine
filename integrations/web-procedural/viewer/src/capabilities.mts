import type {DiagnosticLog} from './errors.mjs';

export type Support = 'supported' | 'fallback' | 'unsupported';

export interface CapabilityRow {
  readonly name: string;
  readonly version: number;
  readonly support: Support;
  /** What this web adapter does with the product. */
  readonly web: string;
  /** What the native Matter renderer does that the web adapter does not. */
  readonly native: string;
}

/** Contract v1 asset capabilities and how this adapter consumes each one. Anything
 *  not listed here (another name or version) is unsupported_capability. */
export const CAPABILITY_MATRIX: readonly CapabilityRow[] = Object.freeze(([
  {name: 'matter.asset.core', version: 1, support: 'supported',
    web: 'Meshes, submeshes, nested assemblies and affine placements; one shared GPU geometry per prototype, InstancedMesh batches for orthogonal placements, individual meshes for sheared ones',
    native: 'Same placements; native additionally flattens/streams sectors'},
  {name: 'matter.material.pbr', version: 1, support: 'supported',
    web: 'Opaque metallic/roughness PBR via MeshStandardMaterial: base color (explicit UV set), packed G=roughness B=metallic, tangent-space normal, emissive; consumer-derived mips',
    native: 'Same inputs; native also bakes virtual-texture chart atlases'},
  {name: 'matter.recipe.core', version: 1, support: 'supported',
    web: 'Recipe capability; carries no asset rendering semantics', native: 'Evaluated by the geometry kernel'},
  {name: 'matter.asset.attachments', version: 1, support: 'supported',
    web: 'Exposed as non-rendering Object3D anchors at instance world * local', native: 'Socket anchors'},
  {name: 'matter.asset.lod', version: 1, support: 'fallback',
    web: 'Base prototypes only; no rung selection, LOD rung meshes are not drawn', native: 'Distance-based rung selection in the compute cull'},
  {name: 'matter.asset.collision', version: 1, support: 'fallback',
    web: 'Not rendered and not simulated', native: 'Physics products'},
  {name: 'matter.asset.animation', version: 1, support: 'fallback',
    web: 'Static bind pose; rigs, skins and clips are not evaluated', native: 'Skinned animation'},
  {name: 'matter.asset.terrain', version: 1, support: 'unsupported',
    web: 'Rejected: terrain regions need sector boundaries and streaming', native: 'Terrain sector streaming and seam stitching'},
] satisfies CapabilityRow[]).map(row => Object.freeze(row)));

export interface NativeOnlyFeature { readonly feature: string; readonly web: string }

/** Native renderer features with no web equivalent. Assets never require them;
 *  listing them keeps the demo honest about what a web view omits. */
export const NATIVE_ONLY_FEATURES: readonly NativeOnlyFeature[] = Object.freeze([
  {feature: 'Ray-traced lighting and shadows (Vulkan RT)', web: 'One hemisphere and one directional light, no shadows'},
  {feature: 'Virtual texturing and chart atlases', web: 'Plain per-material textures'},
  {feature: 'Parallax occlusion mapping', web: 'Tangent-space normal maps only'},
  {feature: 'LOD rung selection and imposters', web: 'Base prototypes only'},
  {feature: 'Vulkan compute cull', web: 'three.js per-object frustum culling'},
  {feature: 'Terrain sector streaming', web: 'Terrain assets are rejected'},
  {feature: 'Retopology and native processing modules', web: 'Assets are shown as evaluated'},
  {feature: 'Transparency and advanced materials (extensions)', web: 'Opaque only; required material extensions are rejected'},
].map(row => Object.freeze(row)));

export type FallbackPolicy = 'label' | 'reject';

interface CapabilityValue { name: unknown; version: unknown }
interface ExtensionValue { capability?: CapabilityValue; required?: unknown }

function key(capability: CapabilityValue): string { return `${String(capability.name)}@${String(capability.version)}`; }

/** Apply the capability policy to a validated asset's declarations. Products
 *  present in the document must be declared; declared capabilities must be known;
 *  fallbacks are labeled (or rejected); required extensions are rejected. */
export function checkCapabilities(asset: Record<string, unknown>, policy: FallbackPolicy, log: DiagnosticLog): Set<string> {
  const declared = asset.required_capabilities;
  if (!Array.isArray(declared)) log.fail('invalid_input', 'required_capabilities must be an array');
  const names = new Set<string>();
  for (const capability of declared as CapabilityValue[]) {
    if (!capability || typeof capability.name !== 'string' || !Number.isInteger(capability.version))
      log.fail('invalid_input', 'capability needs a string name and integer version');
    const row = CAPABILITY_MATRIX.find(r => r.name === capability.name && r.version === capability.version);
    if (!row) {
      const known = CAPABILITY_MATRIX.find(r => r.name === capability.name);
      log.fail('unsupported_capability', known ? `${key(capability)} is not supported (web adapter implements version ${known.version})`
        : `${key(capability)} is unknown to the web adapter`, {capability: key(capability)});
    }
    if (names.has(row.name)) log.fail('invalid_input', `duplicate capability ${key(capability)}`);
    names.add(row.name);
    if (row.support === 'unsupported')
      log.fail('unsupported_capability', `${key(capability)}: ${row.web}`, {capability: key(capability)});
    if (row.support === 'fallback') {
      if (policy === 'reject')
        log.fail('unsupported_capability', `${key(capability)} has only a fallback on the web (${row.web}) and fallbacks are rejected`, {capability: key(capability)});
      log.note('warning', 'fallback', `${key(capability)}: ${row.web}`, {capability: key(capability)});
    }
  }
  const nonEmpty = (field: string) => Array.isArray(asset[field]) && (asset[field] as unknown[]).length > 0;
  const products: [boolean, string, string][] = [
    [true, 'matter.asset.core', 'asset'],
    [nonEmpty('materials') || nonEmpty('textures'), 'matter.material.pbr', 'materials/textures'],
    [nonEmpty('attachments'), 'matter.asset.attachments', 'attachments'],
    [nonEmpty('lods'), 'matter.asset.lod', 'lods'],
    [nonEmpty('collisions'), 'matter.asset.collision', 'collisions'],
    [nonEmpty('rigs') || nonEmpty('skins') || nonEmpty('animations'), 'matter.asset.animation', 'rigs/skins/animations'],
    [asset.terrain !== null && asset.terrain !== undefined, 'matter.asset.terrain', 'terrain'],
  ];
  for (const [present, name, field] of products)
    if (present && !names.has(name)) log.fail('invalid_input', `document carries ${field} but does not declare ${name}@1`, {capability: `${name}@1`});

  if (!Array.isArray(asset.extensions)) log.fail('invalid_input', 'extensions must be an array');
  for (const extension of asset.extensions as ExtensionValue[]) {
    if (!extension?.capability || typeof extension.capability.name !== 'string' || typeof extension.required !== 'boolean')
      log.fail('invalid_input', 'extension envelope needs {capability:{name,version}, required, payload}');
    // The adapter implements no extension, so every required one is unsupported.
    if (extension.required)
      log.fail('unsupported_capability', `required extension ${key(extension.capability)} is not implemented by the web adapter`, {capability: key(extension.capability)});
    log.note('info', 'ignored', `optional extension ${key(extension.capability)} ignored (nonsemantic)`, {capability: key(extension.capability)});
  }
  return names;
}
