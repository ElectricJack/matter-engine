// Parameter/seed demo: fixture picker, parameter controls, seed entry, error
// panel with the last good result kept on screen, diagnostics, capability
// labels and live GPU/runtime statistics. Builds its own DOM in `container`.
import {Box3, DirectionalLight, HemisphereLight, NeutralToneMapping, PerspectiveCamera, Scene, Color, Vector3, WebGLRenderer} from 'three';
import {CAPABILITY_MATRIX, NATIVE_ONLY_FEATURES, type FallbackPolicy} from '../capabilities.mjs';
import type {AssetView} from '../adapter.mjs';
import {FIXTURES, defaults, fixture as fixtureById, type DemoFixture, type JsonObject, type ParameterSpec} from './fixtures.mjs';
import {DemoSession, type DemoFailure, type DemoRuntime, type GenerateOutcome} from './session.mjs';

export interface DemoOptions {
  readonly container: HTMLElement;
  readonly runtime: DemoRuntime;
  /** Repository root URL (asset fixtures fetch contract examples from it). */
  readonly baseURL: URL;
  readonly initialFixture?: string;
  readonly seed?: bigint;
}

export interface DemoState {
  fixture: DemoFixture;
  parameters: JsonObject;
  seed: bigint;
  fallbacks: FallbackPolicy;
}

export interface DemoApp {
  readonly renderer: WebGLRenderer;
  readonly scene: Scene;
  readonly camera: PerspectiveCamera;
  readonly session: DemoSession;
  readonly fixtures: readonly DemoFixture[];
  readonly state: Readonly<DemoState>;
  /** Most recent completed (non-superseded) outcome. */
  readonly last: GenerateOutcome | null;
  /** Error currently displayed, if the latest generation failed. */
  readonly lastError: DemoFailure | null;
  select(id: string): Promise<GenerateOutcome>;
  set(name: string, value: number | boolean): Promise<GenerateOutcome>;
  setSeed(seed: bigint | string): Promise<GenerateOutcome>;
  setFallbacks(policy: FallbackPolicy): Promise<GenerateOutcome>;
  regenerate(): Promise<GenerateOutcome>;
  /** Resolves once no generation is in flight. */
  idle(): Promise<void>;
  frame(): void;
  render(): void;
  dispose(): void;
}

const U64_MAX = (1n << 64n) - 1n;

/** Decimal or 0x-prefixed hexadecimal, inside uint64. */
export function parseSeed(text: string): bigint {
  const trimmed = text.trim();
  if (!/^(0x[0-9a-fA-F]{1,16}|[0-9]{1,20})$/.test(trimmed)) throw new RangeError(`seed "${text}" is not a decimal or 0x-hex integer`);
  const value = BigInt(trimmed);
  if (value > U64_MAX) throw new RangeError(`seed ${trimmed} exceeds uint64`);
  return value;
}

const HINTS: Record<string, string> = {
  invalid_input: 'Check the parameter values and the authoring module.',
  unsupported_capability: 'The asset needs a capability this web view does not implement; see the capability matrix below.',
  unsupported_version: 'The document targets a contract version this web view does not read.',
  missing_dependency: 'A referenced prototype, material, texture or resource is missing.',
  cycle: 'The asset graph references itself.',
  resource_limit: 'A budget was exceeded; reduce the parameter or raise the limit.',
  backend_failure: 'The authoring module threw, failed to load, or the geometry backend failed.',
  disposed: 'The demo was shut down.',
};

function el<K extends keyof HTMLElementTagNameMap>(tag: K, attributes: Record<string, string> = {}, ...children: (Node | string)[]): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attributes)) node.setAttribute(key, value);
  node.append(...children);
  return node;
}

function describe(value: unknown): string {
  if (value && typeof value === 'object') {
    const d = value as {severity?: unknown; code?: unknown; message?: unknown; path?: unknown};
    if (typeof d.message === 'string')
      return [d.severity, d.code].filter(x => typeof x === 'string').join(' ') + `: ${d.message}` + (typeof d.path === 'string' && !d.message.includes(d.path) ? ` (${d.path})` : '');
  }
  return JSON.stringify(value, (_, v: unknown) => typeof v === 'bigint' ? v.toString() : v);
}

export function startDemo(options: DemoOptions): DemoApp {
  const {container, runtime} = options;
  container.replaceChildren();
  container.classList.add('matter-demo');

  // ---- scene ---------------------------------------------------------------
  const viewport = el('div', {class: 'viewport', id: 'viewport'});
  const renderer = new WebGLRenderer({antialias: true, preserveDrawingBuffer: true});
  renderer.toneMapping = NeutralToneMapping;
  renderer.setPixelRatio(Math.min(globalThis.devicePixelRatio || 1, 2));
  renderer.domElement.id = 'canvas';
  viewport.append(renderer.domElement);
  const scene = new Scene();
  scene.background = new Color(0x1d2127);
  const sky = new HemisphereLight(0xf2f5ff, 0x3a3027, 1.6);
  const sun = new DirectionalLight(0xffffff, 2.2);
  sun.position.set(4, 7, 5);
  scene.add(sky, sun);
  const camera = new PerspectiveCamera(40, 1, 0.01, 500);
  const orbit = {target: new Vector3(0, 0.5, 0), radius: 6, yaw: 0.7, pitch: 0.45};

  // ---- controls ------------------------------------------------------------
  const fixtureSelect = el('select', {id: 'fixture', 'aria-label': 'Fixture'},
    ...FIXTURES.map(f => el('option', {value: f.id}, `${f.title}`)));
  const description = el('p', {class: 'description', id: 'fixture-description'});
  const parameterBox = el('div', {class: 'parameters', id: 'parameters'});
  const seedInput = el('input', {id: 'seed', type: 'text', inputmode: 'numeric', spellcheck: 'false', 'aria-label': 'Seed'});
  const randomSeed = el('button', {id: 'random-seed', type: 'button'}, 'Random seed');
  const regenerateButton = el('button', {id: 'regenerate', type: 'button'}, 'Regenerate');
  const policySelect = el('select', {id: 'policy', 'aria-label': 'Fallback policy'},
    el('option', {value: 'label'}, 'Label fallbacks'), el('option', {value: 'reject'}, 'Reject fallbacks'));
  const status = el('div', {id: 'status', class: 'status', role: 'status'});
  const errorPanel = el('section', {id: 'error', class: 'error', role: 'alert'});
  errorPanel.hidden = true;
  const diagnosticsPanel = el('section', {id: 'diagnostics', class: 'diagnostics'});
  const statsTable = el('table', {id: 'stats', class: 'stats'});
  const capabilityTable = el('table', {id: 'capabilities', class: 'capabilities'},
    el('thead', {}, el('tr', {}, el('th', {}, 'Capability'), el('th', {}, 'Web'), el('th', {}, 'Here'), el('th', {}, 'Native'))),
    el('tbody', {}, ...CAPABILITY_MATRIX.map(row => el('tr', {'data-support': row.support},
      el('td', {}, `${row.name}@${row.version}`),
      el('td', {}, el('span', {class: `badge ${row.support}`}, row.support)),
      el('td', {}, row.web), el('td', {}, row.native)))));
  const nativeTable = el('table', {id: 'native-only', class: 'capabilities'},
    el('thead', {}, el('tr', {}, el('th', {}, 'Native-only feature'), el('th', {}, 'Web view'))),
    el('tbody', {}, ...NATIVE_ONLY_FEATURES.map(row => el('tr', {},
      el('td', {}, el('span', {class: 'badge native'}, 'native only'), ' ', row.feature), el('td', {}, row.web)))));

  const controls = el('aside', {class: 'controls'},
    el('h1', {}, 'Matter procedural web view'),
    el('label', {class: 'field'}, 'Fixture', fixtureSelect), description,
    el('h2', {}, 'Parameters'), parameterBox,
    el('h2', {}, 'Seed'), el('div', {class: 'row'}, seedInput, randomSeed),
    el('label', {class: 'field'}, 'Fallbacks', policySelect),
    el('div', {class: 'row'}, regenerateButton),
    status, errorPanel, diagnosticsPanel,
    el('h2', {}, 'Statistics'), statsTable);
  const reference = el('section', {class: 'reference'},
    el('h2', {}, 'Capability matrix'), capabilityTable,
    el('h2', {}, 'Native renderer features not available here'), nativeTable);
  container.append(controls, el('main', {}, viewport, reference));

  const session = new DemoSession(runtime, scene, {baseURL: options.baseURL});
  const initial = fixtureById(options.initialFixture ?? FIXTURES[0]!.id);
  const state: DemoState = {fixture: initial, parameters: defaults(initial), seed: options.seed ?? 1n, fallbacks: 'label'};
  let last: GenerateOutcome | null = null;
  let lastError: DemoFailure | null = null;
  let lastGood: {fixture: string; epoch: number} | null = null;
  let inflight = 0;
  let idleWaiters: (() => void)[] = [];
  let reframe = true;
  let disposed = false;

  // ---- rendering -----------------------------------------------------------
  let frameRequested = false;
  function render(): void {
    frameRequested = false;
    if (disposed) return;
    const {target, radius, yaw, pitch} = orbit;
    camera.position.set(target.x + radius * Math.cos(pitch) * Math.sin(yaw), target.y + radius * Math.sin(pitch),
      target.z + radius * Math.cos(pitch) * Math.cos(yaw));
    camera.lookAt(target.x, target.y, target.z);
    renderer.render(scene, camera);
    updateStats();
  }
  const requestRender = () => { if (!frameRequested && !disposed) { frameRequested = true; requestAnimationFrame(render); } };
  function resize(): void {
    const width = Math.max(1, viewport.clientWidth), height = Math.max(1, viewport.clientHeight);
    renderer.setSize(width, height, false);
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
    requestRender();
  }
  const observer = new ResizeObserver(resize);
  observer.observe(viewport);

  function frame(): void {
    const view = session.view;
    if (!view) return;
    view.root.updateMatrixWorld(true);
    const box = new Box3().setFromObject(view.root);
    if (box.isEmpty()) return;
    const size = box.getSize(new Vector3()).length();
    box.getCenter(orbit.target);
    orbit.radius = Math.max(0.5, size * 0.5 / Math.tan(camera.fov * Math.PI / 360) * 1.15);
    camera.near = orbit.radius / 200; camera.far = orbit.radius * 20;
    camera.updateProjectionMatrix();
    requestRender();
  }

  // Minimal orbit: drag rotates, wheel zooms.
  let drag: {x: number; y: number; id: number} | null = null;
  const onDown = (e: PointerEvent) => { drag = {x: e.clientX, y: e.clientY, id: e.pointerId}; renderer.domElement.setPointerCapture(e.pointerId); };
  const onMove = (e: PointerEvent) => {
    if (!drag || e.pointerId !== drag.id) return;
    orbit.yaw -= (e.clientX - drag.x) * 0.008;
    orbit.pitch = Math.min(1.5, Math.max(-1.5, orbit.pitch + (e.clientY - drag.y) * 0.008));
    drag.x = e.clientX; drag.y = e.clientY;
    requestRender();
  };
  const onUp = () => { drag = null; };
  const onWheel = (e: WheelEvent) => { e.preventDefault(); orbit.radius *= Math.exp(e.deltaY * 0.001); requestRender(); };
  renderer.domElement.addEventListener('pointerdown', onDown);
  renderer.domElement.addEventListener('pointermove', onMove);
  renderer.domElement.addEventListener('pointerup', onUp);
  renderer.domElement.addEventListener('wheel', onWheel, {passive: false});

  // ---- panels --------------------------------------------------------------
  function buildParameters(): void {
    parameterBox.replaceChildren();
    const entries = Object.entries(state.fixture.parameters) as [string, ParameterSpec][];
    if (entries.length === 0) parameterBox.append(el('p', {class: 'muted'}, 'This fixture has no parameters.'));
    for (const [name, spec] of entries) {
      const value = state.parameters[name];
      if (spec.type === 'boolean') {
        const box = el('input', {id: `param-${name}`, type: 'checkbox', 'data-parameter': name});
        box.checked = value === true;
        box.addEventListener('change', () => void set(name, box.checked));
        parameterBox.append(el('label', {class: 'field check'}, box, spec.label));
        continue;
      }
      const step = String(spec.step ?? 1);
      const bounds = {min: String(spec.minimum ?? ''), max: String(spec.maximum ?? ''), step};
      const number = el('input', {id: `param-${name}`, type: 'number', ...bounds, 'data-parameter': name});
      const range = el('input', {id: `param-${name}-range`, type: 'range', ...bounds, tabindex: '-1', 'aria-hidden': 'true'});
      number.value = range.value = String(value);
      const commit = (text: string) => {
        const parsed = spec.type === 'integer' ? Number.parseInt(text, 10) : Number.parseFloat(text);
        number.value = range.value = text;
        void set(name, parsed);
      };
      number.addEventListener('change', () => commit(number.value));
      range.addEventListener('input', () => commit(range.value));
      parameterBox.append(el('label', {class: 'field'}, el('span', {}, spec.label), el('div', {class: 'row'}, range, number)));
    }
  }

  function showStatus(text: string, kind: 'busy' | 'ok' | 'error'): void {
    status.textContent = text;
    status.dataset.kind = kind;
  }

  function showError(error: DemoFailure | null): void {
    lastError = error;
    errorPanel.hidden = error === null;
    if (!error) { errorPanel.replaceChildren(); return; }
    const kept = session.view && lastGood
      ? `Showing the last good result (${lastGood.fixture}, generation ${lastGood.epoch}).` : 'Nothing to show yet.';
    errorPanel.replaceChildren(
      el('h2', {}, el('span', {class: 'code', id: 'error-code'}, error.code), ' ', el('span', {class: 'stage', id: 'error-stage'}, `${error.stage} stage`)),
      el('p', {class: 'message', id: 'error-message'}, error.message),
      ...(HINTS[error.code] ? [el('p', {class: 'hint'}, HINTS[error.code]!)] : []),
      ...(error.diagnostics.length ? [el('ul', {class: 'error-diagnostics'}, ...error.diagnostics.map(d => el('li', {}, describe(d))))] : []),
      el('p', {class: 'kept', id: 'error-kept'}, kept));
  }

  function showDiagnostics(view: AssetView | null): void {
    diagnosticsPanel.replaceChildren();
    if (!view) return;
    const items = view.diagnostics.map(d => el('li', {'data-severity': d.severity, 'data-code': d.code},
      el('span', {class: `badge ${d.code === 'fallback' ? 'fallback' : d.severity}`}, d.code === 'fallback' ? 'web fallback' : d.code),
      ' ', d.capability ? `${d.message}` : d.message));
    const anchors = [...view.attachments.keys()].map(id => {
      // Anchors carry an authoritative matrix (matrixAutoUpdate off); position is not maintained.
      const p = new Vector3().setFromMatrixPosition(view.attachments.get(id)!.matrix);
      return el('li', {'data-code': 'attachment'}, el('span', {class: 'badge info'}, 'anchor'), ` ${id} at (${[p.x, p.y, p.z].map(v => v.toFixed(3)).join(', ')})`);
    });
    if (items.length + anchors.length === 0) return;
    diagnosticsPanel.append(el('h2', {}, 'Diagnostics'), el('ul', {}, ...items, ...anchors));
  }

  function row(name: string, value: string | number): HTMLTableRowElement {
    return el('tr', {'data-stat': name}, el('th', {}, name), el('td', {}, String(value)));
  }
  function updateStats(): void {
    const view = session.view, info = renderer.info, s = view?.stats;
    const rt = runtime.stats as Record<string, unknown>;
    statsTable.replaceChildren(
      row('prototypes', s?.prototypes ?? 0), row('instances', s?.instances ?? 0), row('draw objects', s?.drawObjects ?? 0),
      row('batches (instanced / mirrored / individual)', s ? `${s.batches.instanced} / ${s.batches.mirroredInstanced} / ${s.batches.individual}` : '0 / 0 / 0'),
      row('geometries (view)', s?.geometries ?? 0), row('materials', s?.materials ?? 0), row('textures', s?.textures ?? 0),
      row('triangles drawn / uploaded', s ? `${s.triangles} / ${s.uploadedTriangles}` : '0 / 0'),
      row('GPU geometries', info.memory.geometries), row('GPU textures', info.memory.textures),
      row('programs', info.programs?.length ?? 0), row('draw calls', info.render.calls),
      row('runtime workers / restarts', `${String(rt.workers ?? '?')} / ${String(rt.restarts ?? '?')}`));
  }

  // ---- generation ----------------------------------------------------------
  async function regenerate(): Promise<GenerateOutcome> {
    inflight++;
    showStatus(`Generating ${state.fixture.id}…`, 'busy');
    try {
      const outcome = await session.generate({fixture: state.fixture, parameters: {...state.parameters}, seed: state.seed,
        viewOptions: {fallbacks: state.fallbacks}});
      if (outcome.status === 'superseded') return outcome;
      last = outcome;
      if (outcome.status === 'ok') {
        lastGood = {fixture: state.fixture.id, epoch: outcome.epoch};
        showError(null);
        showDiagnostics(outcome.view);
        const runtimeText = outcome.runtimeMs === null ? '' : ` (runtime ${outcome.runtimeMs.toFixed(1)} ms)`;
        showStatus(`${state.fixture.id} · generation ${outcome.epoch} · ${outcome.elapsedMs.toFixed(1)} ms${runtimeText}`, 'ok');
        if (reframe) { reframe = false; frame(); }
      } else {
        showError(outcome.error);
        showStatus(`${state.fixture.id} · generation ${outcome.epoch} failed: ${outcome.error.code}`, 'error');
      }
      requestRender();
      return outcome;
    } finally {
      if (--inflight === 0) { const waiters = idleWaiters; idleWaiters = []; waiters.forEach(w => w()); }
    }
  }

  function select(id: string): Promise<GenerateOutcome> {
    state.fixture = fixtureById(id);
    state.parameters = defaults(state.fixture);
    fixtureSelect.value = id;
    description.textContent = state.fixture.description;
    reframe = true;
    buildParameters();
    return regenerate();
  }
  function set(name: string, value: number | boolean): Promise<GenerateOutcome> {
    if (!Object.hasOwn(state.fixture.parameters, name)) throw new RangeError(`${state.fixture.id} has no parameter ${name}`);
    state.parameters = {...state.parameters, [name]: value};
    const input = container.querySelector<HTMLInputElement>(`#param-${name}`);
    if (input) { if (input.type === 'checkbox') input.checked = value === true; else input.value = String(value); }
    return regenerate();
  }
  async function setSeed(seed: bigint | string): Promise<GenerateOutcome> {
    let value: bigint;
    try { value = typeof seed === 'bigint' ? parseSeed(seed.toString()) : parseSeed(seed); }
    catch (error) {
      const failure: DemoFailure = {stage: 'input', code: 'invalid_input', message: (error as Error).message, diagnostics: []};
      showError(failure);
      showStatus(`seed rejected: ${failure.message}`, 'error');
      const outcome: GenerateOutcome = {status: 'error', epoch: session.epoch, error: failure};
      last = outcome;
      return outcome;
    }
    state.seed = value;
    seedInput.value = value.toString();
    return regenerate();
  }
  function setFallbacks(policy: FallbackPolicy): Promise<GenerateOutcome> {
    state.fallbacks = policy;
    policySelect.value = policy;
    return regenerate();
  }

  fixtureSelect.addEventListener('change', () => void select(fixtureSelect.value));
  seedInput.addEventListener('change', () => void setSeed(seedInput.value));
  randomSeed.addEventListener('click', () => {
    const words = crypto.getRandomValues(new BigUint64Array(1));
    void setSeed(words[0]!);
  });
  regenerateButton.addEventListener('click', () => void regenerate());
  policySelect.addEventListener('change', () => void setFallbacks(policySelect.value as FallbackPolicy));

  seedInput.value = state.seed.toString();
  const first = select(initial.id);
  void first;

  return {
    renderer, scene, camera, session, fixtures: FIXTURES,
    get state() { return state; }, get last() { return last; }, get lastError() { return lastError; },
    select, set, setSeed, setFallbacks, regenerate,
    idle: () => inflight === 0 ? Promise.resolve() : new Promise<void>(resolve => idleWaiters.push(resolve)),
    frame, render,
    dispose() {
      if (disposed) return;
      disposed = true;
      observer.disconnect();
      renderer.domElement.removeEventListener('pointerdown', onDown);
      renderer.domElement.removeEventListener('pointermove', onMove);
      renderer.domElement.removeEventListener('pointerup', onUp);
      renderer.domElement.removeEventListener('wheel', onWheel);
      session.dispose();
      renderer.dispose();
    },
  };
}
