// Hand-authored wire expectations; never regenerate these from the SDK.
const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
const common = { document_type: 'recipe', canonical_profile: 'matter-exact-json-v1', schema_version: { major: 1, minor: 0 },
  resources: [], textures: [], extensions: [], metadata: {}, source_map: [], rng_algorithm: 'matter.splitmix64.v1' };
export const expectedProp = { ...common,
  source: { id: 'example.prop', uri: 'example://prop', revision: '1', content: null, legacy_hashes: [] }, seed: '0000000000000001',
  parameters: { depth: 2, enabled: true }, required_capabilities: [{ name: 'matter.recipe.core', version: 1 }, { name: 'matter.material.pbr', version: 1 }],
  source_map: {}, asset_dependencies: [], materials: [{ id: 'clay', base_color: { factor: [1, 1, 1, 1], texture: null, uv_set: 0 },
    metallic: 0, roughness: 0.800000011920929, normal_scale: 1, emissive: [0, 0, 0], metallic_roughness_texture: null, normal_texture: null, emissive_texture: null }],
  nodes: [
    { id: 'profile', operation: 'profile.polygon', operation_version: 1, output_type: 'profile', inputs: [], parameters: { points: [[0, 0], [1, 0], [1, 1], [0, 1]] }, transform: identity, materials: [] },
    { id: 'prop', operation: 'profile.extrude', operation_version: 1, output_type: 'mesh', inputs: [{ port: 'profile', kind: 'node', reference: 'profile', type: 'profile' }], parameters: { depth: 2 }, transform: identity, materials: ['clay'] }
  ], outputs: ['prop'] };
export const expectedRepeated = { ...common,
  source: { id: 'example.repeated', uri: 'example://repeated', revision: '1', content: null, legacy_hashes: [] }, seed: '0000000000000000',
  parameters: {}, required_capabilities: [{ name: 'matter.recipe.core', version: 1 }], source_map: {}, materials: [],
  asset_dependencies: [{ id: 'shared.pair', content: { algorithm: 'sha256', hex: 'abababababababababababababababababababababababababababababababab' }, schema_version: { major: 1, minor: 0 } }],
  nodes: [
    { id: 'pair', operation: 'assembly.reference', operation_version: 1, output_type: 'assembly', inputs: [{ port: 'assembly', kind: 'asset', reference: 'shared.pair', type: 'assembly' }], parameters: {}, transform: identity, materials: [] },
    { id: 'placed.0', operation: 'placement.affine', operation_version: 1, output_type: 'assembly', inputs: [{ port: 'input', kind: 'node', reference: 'pair', type: 'assembly' }], parameters: {}, transform: identity, materials: [] },
    { id: 'placed.1', operation: 'placement.affine', operation_version: 1, output_type: 'assembly', inputs: [{ port: 'input', kind: 'node', reference: 'pair', type: 'assembly' }], parameters: {}, transform: [-2, 0, 0, 3, 0, 3, 0, 2, 0, 0, 4, 1, 0, 0, 0, 1], materials: [] }
  ], outputs: ['placed.0', 'placed.1'] };
