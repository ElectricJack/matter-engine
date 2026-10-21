import { RecipeBuilder, parameters, DEFAULT_CAPABILITIES, translation } from '../../../../packages/matter-authoring/index.mjs';
import type { Matrix, NodeRef, RecipeDocument, Vec2, Vec3 } from '../../../../packages/matter-authoring/index.mjs';

export type NumericParameter = {
  readonly type: 'number' | 'integer'; readonly default: number;
  readonly minimum: number; readonly maximum: number; readonly step: number;
  readonly label: string; readonly unit?: string;
};
export type ParameterValues = Readonly<Record<string, number>>;
export interface RecipeDefinition {
  readonly id: string; readonly name: string; readonly description: string;
  readonly parameters: Readonly<Record<string, NumericParameter>>;
  build(overrides?: ParameterValues, seed?: string): RecipeDocument;
}
export const number = (label: string, value: number, minimum: number, maximum: number, step = 0.05, unit = 'm'): NumericParameter =>
  Object.freeze({ type: 'number', label, default: value, minimum, maximum, step, unit });
export const integer = (label: string, value: number, minimum: number, maximum: number): NumericParameter =>
  Object.freeze({ type: 'integer', label, default: value, minimum, maximum, step: 1 });
const operations = ['primitive.cylinder', 'primitive.cone', 'profile.contours', 'mesh.triangles', 'solid.ordered'];
const supportedCapabilities = [...DEFAULT_CAPABILITIES, ...operations.map(operation => ({ name: `matter.operation.${operation}`, version: 1 }))];

export function defineRecipe(id: string, name: string, description: string,
  controls: Record<string, NumericParameter>, author: (b: RecipeBuilder, p: ParameterValues, seed: string) => void): RecipeDefinition {
  const frozenControls = Object.freeze({ ...controls });
  return Object.freeze({ id, name, description, parameters: frozenControls,
    build(overrides: ParameterValues = {}, seed = '0000000000000001') {
      const specs = Object.fromEntries(Object.entries(frozenControls).map(([key, p]) =>
        [key, { type: p.type, default: p.default, minimum: p.minimum, maximum: p.maximum }]));
      const p = parameters(specs, overrides);
      const b = new RecipeBuilder({ source: { id: `demo.${id}`, uri: `recipe://${id}`, revision: '1' }, seed, parameters: p, supportedCapabilities });
      b.metadata({ name, description, upAxis: 'y', units: 'metres' });
      author(b, p, seed);
      return b.build();
    },
  });
}
export function material(b: RecipeBuilder, color: readonly [number, number, number], metallic = 0, roughness = 0.65) {
  return b.material('surface', { baseColor: [...color, 1], metallic, roughness });
}
export function box(b: RecipeBuilder, id: string, size: Vec3, center: Vec3, surface = 'surface') {
  const mesh = b.box(`${id}.mesh`, size, { materials: [surface] });
  return b.place(id, mesh, translation(...center));
}
export function round(b: RecipeBuilder, id: string, a: Vec3, c: Vec3, radius0: number, radius1 = radius0, segments = 12) {
  return b.node({ id, operation: radius0 === radius1 ? 'primitive.cylinder' : 'primitive.cone',
    parameters: { a, b: c, radius0, radius1, segments }, materials: ['surface'] });
}
// Store preview resolution in the document, so every evaluator uses the same
// lattice and the UI does not need application-specific geometry settings.
export function csg(b: RecipeBuilder, id: string, operator: 'union' | 'subtract', operands: readonly NodeRef<'mesh'>[], spacing = 0.15) {
  return b.node({ id, operation: 'solid.ordered', inputs: operands.map(ref => ({ ...ref, port: 'operand' })),
    parameters: { operations: operands.map((_, i) => i === 0 ? 'union' : operator), spacing, smoothing: 0, referenceRadius: 0.1 }, materials: ['surface'] });
}
export function extruded(b: RecipeBuilder, id: string, points: readonly Vec2[], depth: number,
  holes: readonly (readonly Vec2[])[] = [], transform?: Matrix) {
  const profile = holes.length ? b.node<'profile'>({ id: `${id}.profile`, operation: 'profile.contours', outputType: 'profile', parameters: { points, holes } }) : b.polygon(`${id}.profile`, points);
  const mesh = b.extrude(id, profile, depth, { materials: ['surface'] });
  return transform ? b.place(`${id}.placed`, mesh, transform) : mesh;
}
export const circle = (radius: number, count = 24): Vec2[] => Array.from({ length: count }, (_, i) =>
  [radius * Math.cos(i * 2 * Math.PI / count), radius * Math.sin(i * 2 * Math.PI / count)]);
export function triangles(b: RecipeBuilder, id: string, positions: readonly Vec3[]): NodeRef<'mesh'> {
  return b.node({ id, operation: 'mesh.triangles', parameters: { positions, mode: 0 }, materials: ['surface'] });
}

// Revolve a closed radial/height profile. Consecutive axis points omit zero-area
// faces; the resulting triangle soup has a welded closed seam and cap poles.
export function lathe(b: RecipeBuilder, id: string, profile: readonly Vec2[], segments: number) {
  const positions: Vec3[] = [];
  const point = ([r, y]: Vec2, angle: number): Vec3 => [r * Math.cos(angle), y, r * Math.sin(angle)];
  for (let j = 0; j < profile.length; j++) {
    const a = profile[j], c = profile[(j + 1) % profile.length];
    for (let i = 0; i < segments; i++) {
      const t0 = i * 2 * Math.PI / segments, t1 = (i + 1) * 2 * Math.PI / segments;
      const v0 = point(a, t0), v1 = point(a, t1), v2 = point(c, t0), v3 = point(c, t1);
      if (a[0] > 0) positions.push(v0, v2, v1);
      if (c[0] > 0) positions.push(v1, v2, v3);
    }
  }
  return triangles(b, id, positions);
}
