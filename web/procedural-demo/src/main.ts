import GUI from 'lil-gui';
import { recipes, recipeById, type RecipeDefinition } from './recipes/index.ts';
import { constructionStages, type ConstructionStage } from './stages.ts';
import { EvaluationController, loadEngine, type EvaluationState } from './evaluation.ts';
import { Viewport } from './viewport.ts';
import './style.css';

const element = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id)! as T;
const timeline = element<HTMLInputElement>('timeline'), status = element('status');
const seed = element<HTMLInputElement>('seed');
const wireframe = element<HTMLInputElement>('wireframe'), normals = element<HTMLInputElement>('normals');
let selected: RecipeDefinition = recipeById.get('arch')!;
let values: Record<string, number> = {};
let stages: readonly ConstructionStage[] = [];
let step = 0, timer: ReturnType<typeof setTimeout> | undefined;
let gui: GUI | undefined, viewport: Viewport | undefined, evaluator: EvaluationController | undefined;
let frameNext = true, disposed = false;

const setStatus = (message: string, kind = 'ready') => {
  status.textContent = message; status.dataset.kind = kind;
  element('app').dataset.state = kind;
};

function showError(error: unknown): void {
  const value = error as { message?: string; code?: string; diagnostics?: unknown };
  setStatus(`${value.code ? `${value.code}: ` : ''}${value.message ?? String(error)}`, 'error');
  element('diagnostics').textContent = JSON.stringify(value.diagnostics ?? value.message ?? String(error), null, 2);
  element<HTMLDetailsElement>('diagnostics-panel').open = true;
  // Old geometry must not masquerade as the newly requested recipe/stage.
  viewport?.clear();
  for (const id of ['eval-time', 'upload-time', 'triangles', 'instances', 'heap']) element(id).textContent = '—';
}

function publish(state: EvaluationState): void {
  if (state.kind === 'busy') { setStatus('Evaluating construction…', 'busy'); return; }
  if (state.kind === 'error') { showError(state.error); return; }
  try {
    const { result } = state;
    const metrics = viewport!.show(result, frameNext);
    // Profile-only stages have no drawable object: keep framing requested for the next solid.
    if (result.instances.length) frameNext = false;
    element('eval-time').textContent = `${result.measurements.elapsedMs.toFixed(1)} ms${state.cached ? ' · cached' : ''}`;
    element('upload-time').textContent = `${metrics.uploadMs.toFixed(1)} ms`;
    element('triangles').textContent = metrics.triangles.toLocaleString();
    element('instances').textContent = result.instances.length.toLocaleString();
    element('heap').textContent = `${(result.measurements.wasmHeapBytes / 1048576).toFixed(1)} MB`;
    element('diagnostics').textContent = result.diagnostics.length ? JSON.stringify(result.diagnostics, null, 2) : 'No diagnostics.';
    element('app').dataset.recipe = selected.id;
    element('app').dataset.step = String(step);
    setStatus(result.instances.length ? 'Ready' : 'Profile step · no solid geometry yet');
  } catch (error) { showError(error); }
}

function updateTimeline(): void {
  timeline.max = String(Math.max(stages.length - 1, 0)); timeline.value = String(step);
  element('step-count').textContent = `${String(step + 1).padStart(2, '0')} / ${String(stages.length).padStart(2, '0')}`;
  element('step-name').textContent = stages[step]?.name ?? '';
  element('step-description').textContent = stages[step]?.description ?? '';
  element<HTMLButtonElement>('previous').disabled = step === 0;
  element<HTMLButtonElement>('next').disabled = step === stages.length - 1;
  timeline.setAttribute('aria-valuetext', stages[step]?.name ?? '');
}

function evaluateSelected(): void {
  clearTimeout(timer);
  if (!evaluator || !stages[step]) return;
  if (stages[step].recipe.outputs.length === 0) {
    // A profile-only prefix is educational, not a valid mesh evaluation request.
    evaluator.invalidate(); viewport?.clear();
    for (const id of ['eval-time', 'upload-time', 'triangles', 'instances', 'heap']) element(id).textContent = '—';
    element('diagnostics').textContent = '2D profile prepared; choose an extrusion step to preview solid geometry.';
    element('app').dataset.recipe = selected.id; element('app').dataset.step = String(step);
    setStatus('Profile step · no solid geometry yet');
    return;
  }
  void evaluator.evaluate(step, stages[step].recipe);
}

function rebuild(debounce = true): void {
  clearTimeout(timer); evaluator?.invalidate();
  try {
    if (!/^[0-9a-fA-F]{16}$/.test(seed.value)) throw new Error('Enter a seed with exactly 16 hexadecimal digits.');
    seed.setCustomValidity('');
    const wasFinal = step === stages.length - 1;
    stages = constructionStages(selected.build(values, seed.value.toLowerCase()));
    step = wasFinal ? stages.length - 1 : Math.min(step, stages.length - 1);
    updateTimeline();
    if (evaluator) {
      setStatus('Updating recipe…', 'busy');
      if (debounce) timer = setTimeout(evaluateSelected, 150);
      else evaluateSelected();
    }
  } catch (error) {
    if (!/^[0-9a-fA-F]{16}$/.test(seed.value)) seed.setCustomValidity('Use 16 hexadecimal digits.');
    showError(error);
  }
}

function select(recipe: RecipeDefinition): void {
  selected = recipe; frameNext = true;
  viewport?.clear(); evaluator?.invalidate(); clearTimeout(timer);
  element('object-name').textContent = recipe.name;
  element('description').textContent = recipe.description;
  document.querySelectorAll<HTMLButtonElement>('#gallery button').forEach(button => {
    button.setAttribute('aria-pressed', String(button.dataset.recipe === recipe.id));
  });
  values = Object.fromEntries(Object.entries(recipe.parameters).map(([key, spec]) => [key, spec.default]));
  gui?.destroy();
  gui = new GUI({ container: element('parameters'), title: 'Object dimensions' });
  for (const [key, spec] of Object.entries(recipe.parameters)) {
    gui.add(values, key, spec.minimum, spec.maximum, spec.step)
      .name(`${spec.label}${spec.unit ? ` (${spec.unit})` : ''}`).onChange(() => rebuild());
  }
  // Hide only the folder header, leaving lil-gui's labelled input controls intact.
  gui.domElement.classList.add('parameter-gui');
  stages = []; step = -1; rebuild(false);
  const url = new URL(location.href); url.searchParams.set('object', recipe.id);
  history.replaceState(null, '', url);
}

function chooseStep(value: number): void {
  step = Math.max(0, Math.min(value, stages.length - 1));
  updateTimeline(); evaluateSelected();
}

const gallerySymbols = ['◇', '♧', '▥', '∩', '⚙', '◡', '▱', '♢', '⌁', '∞', '△', '▦', '⌞'];
recipes.forEach((recipe, index) => {
  const button = document.createElement('button'); button.type = 'button'; button.dataset.recipe = recipe.id;
  const symbol = document.createElement('span'); symbol.className = 'object-symbol'; symbol.textContent = gallerySymbols[index] ?? '◇';
  const label = document.createElement('span'); label.textContent = recipe.name;
  const number = document.createElement('span'); number.className = 'object-number'; number.textContent = String(index + 1).padStart(2, '0');
  button.append(symbol, label, number); button.addEventListener('click', () => select(recipe));
  element('gallery').append(button);
});
element('gallery-count').textContent = String(recipes.length).padStart(2, '0');
timeline.addEventListener('input', () => chooseStep(Number(timeline.value)));
element('previous').addEventListener('click', () => chooseStep(step - 1));
element('next').addEventListener('click', () => chooseStep(step + 1));
element('final').addEventListener('click', () => chooseStep(stages.length - 1));
element('frame').addEventListener('click', () => viewport?.frame());
element('reset').addEventListener('click', () => { seed.value = '0000000000000001'; select(selected); });
seed.addEventListener('input', () => rebuild());
const display = () => viewport?.setDisplay(wireframe.checked, normals.checked);
wireframe.addEventListener('change', display); normals.addEventListener('change', display);

function dispose(): void {
  if (disposed) return;
  disposed = true; clearTimeout(timer); evaluator?.dispose(); viewport?.dispose(); gui?.destroy();
}
addEventListener('pagehide', dispose, { once: true });
import.meta.hot?.dispose(dispose);

select(recipeById.get(new URL(location.href).searchParams.get('object') ?? '') ?? selected);
try {
  viewport = new Viewport(element<HTMLCanvasElement>('canvas'), element('viewport'));
  display();
  const engine = await loadEngine();
  if (disposed) engine.dispose();
  else { evaluator = new EvaluationController(engine, publish); rebuild(false); }
} catch (error) { showError(error); }
