import type { ProceduralEngine, EvaluationResult, RecipeDocument } from '../../matter-procedural-wasm/index.d.ts';

export type { EvaluationResult, ProceduralEngine };
export type EvaluationState =
  | { kind: 'busy' }
  | { kind: 'ready'; result: EvaluationResult; cached: boolean }
  | { kind: 'error'; error: unknown };

/** Evaluate only the selected stage. Bound JS-owned result retention and reject stale completions. */
export class EvaluationController {
  private controller: AbortController | undefined;
  private generation = 0;
  private disposed = false;
  private readonly cache = new Map<number, EvaluationResult>();
  private readonly engine: ProceduralEngine;
  private readonly publish: (state: EvaluationState) => void;

  constructor(engine: ProceduralEngine, publish: (state: EvaluationState) => void) {
    this.engine = engine; this.publish = publish;
  }

  invalidate(): void {
    this.generation++;
    this.controller?.abort();
    this.controller = undefined;
    this.cache.clear();
  }

  async evaluate(stage: number, recipe: RecipeDocument): Promise<void> {
    if (this.disposed) return;
    const generation = ++this.generation;
    this.controller?.abort();
    const cached = this.cache.get(stage);
    if (cached) {
      this.cache.delete(stage); this.cache.set(stage, cached);
      this.publish({ kind: 'ready', result: cached, cached: true });
      return;
    }
    const controller = new AbortController();
    this.controller = controller;
    this.publish({ kind: 'busy' });
    try {
      const result = await this.engine.evaluate(recipe, {
        signal: controller.signal,
        budget: { operations: 20_000_000, vertices: 200_000, indices: 600_000 },
      });
      if (this.disposed || generation !== this.generation) return;
      this.cache.set(stage, result);
      if (this.cache.size > 4) this.cache.delete(this.cache.keys().next().value!);
      this.publish({ kind: 'ready', result, cached: false });
    } catch (error) {
      if (!this.disposed && generation === this.generation) this.publish({ kind: 'error', error });
    }
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true; this.invalidate(); this.engine.dispose();
  }
}

export async function loadEngine(): Promise<ProceduralEngine> {
  const url = new URL(`${import.meta.env.BASE_URL}matter-procedural-wasm/index.mjs`, document.baseURI).href;
  // A public package must stay intact; bundling it would break worker/module URLs.
  const { createProceduralEngine } = await import(/* @vite-ignore */ url) as {
    createProceduralEngine(): Promise<ProceduralEngine>;
  };
  return createProceduralEngine();
}
