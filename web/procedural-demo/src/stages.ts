import type { RecipeDocument, RecipeNode } from '../../../packages/matter-authoring/index.mjs';

export interface ConstructionStage {
  readonly name: string;
  readonly description: string;
  readonly recipe: RecipeDocument;
}

/** Dependency order, including forward references; only nodes reachable from the outputs. */
export function constructionStages(document: RecipeDocument): readonly ConstructionStage[] {
  const nodes = new Map(document.nodes.map(node => [node.id, node]));
  const visited = new Set<string>(), visiting = new Set<string>(), ordered: RecipeNode[] = [];
  const visit = (id: string) => {
    if (visited.has(id)) return;
    if (visiting.has(id)) throw new Error(`Cyclic construction dependency: ${id}`);
    const node = nodes.get(id);
    if (!node) throw new Error(`Missing construction node: ${id}`);
    visiting.add(id);
    for (const input of node.inputs) if (input.kind === 'node') visit(input.reference);
    visiting.delete(id); visited.add(id); ordered.push(node);
  };
  document.outputs.forEach(visit);
  const frontier = new Set<string>();
  const stages: ConstructionStage[] = [];
  const prefix: RecipeNode[] = [];
  const prefixIds = new Set<string>();
  for (const node of ordered) {
    prefix.push(node); prefixIds.add(node.id);
    // Profiles are inputs, never renderable mesh outputs. Their creation still has a step.
    if (node.output_type !== 'profile') {
      for (const input of node.inputs) if (input.kind === 'node') frontier.delete(input.reference);
      frontier.add(node.id);
    }
    const operations = node.parameters.operations;
    const operation = Array.isArray(operations) ? `${node.operation}: ${operations.join(' → ')}` : node.operation;
    stages.push({
      name: node.id.replaceAll('.', ' / '),
      description: `${operation}${node.output_type === 'profile' ? ' · 2D profile prepared for extrusion; solid preview appears at the next mesh step.' : ''}`,
      recipe: { ...document, nodes: [...prefix], outputs: [...frontier],
        source_map: Object.fromEntries(Object.entries(document.source_map).filter(([id]) => prefixIds.has(id))) },
    });
  }
  // The exact original outputs matter: assembly prototype construction can leave
  // unplaced intermediates in the frontier. Final must be the author's document.
  stages.push({ name: 'Final object', description: `${document.outputs.length} authored output${document.outputs.length === 1 ? '' : 's'} · complete recipe`, recipe: document });
  return stages;
}
