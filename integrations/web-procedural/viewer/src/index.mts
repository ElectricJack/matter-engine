// Web renderer adapter for Matter contract v1 assets. Depends on three.js
// (vendored, mapped through an import map); the procedural runtime and the
// authoring SDK never import this module.
export {createAssetView, DEFAULT_MAX_DEPTH, DEFAULT_MAX_INSTANCES} from './adapter.mjs';
export type {AssetSource, AssetView, AssetViewOptions, AssetViewStats, MeshBuffers, PlacedInstance} from './adapter.mjs';
export {CAPABILITY_MATRIX, NATIVE_ONLY_FEATURES, checkCapabilities} from './capabilities.mjs';
export type {CapabilityRow, FallbackPolicy, NativeOnlyFeature, Support} from './capabilities.mjs';
export {DiagnosticLog, ViewerError} from './errors.mjs';
export type {DiagnosticSeverity, ViewerDiagnostic, ViewerErrorCode} from './errors.mjs';
export {DEFAULT_MAX_TEXTURE_SIZE, MAX_UV_SET} from './materials.mjs';
export type {MaterialOptions, ResourceDescriptor, ResourceResolver} from './materials.mjs';
export {IDENTITY, MIRROR_X, classify, determinant3, multiply} from './matrix.mjs';
export type {Affine, PlacementClass} from './matrix.mjs';
