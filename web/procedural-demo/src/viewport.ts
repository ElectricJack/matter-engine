import { ACESFilmicToneMapping, Box3, Color, DirectionalLight, GridHelper, HemisphereLight, MathUtils, PerspectiveCamera, PMREMGenerator, Scene, Vector3, WebGLRenderer } from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { RoomEnvironment } from 'three/addons/environments/RoomEnvironment.js';
import { createMeshView, type MeshView } from './mesh-view.ts';
import type { EvaluationResult } from './evaluation.ts';

export class Viewport {
  private readonly renderer: WebGLRenderer;
  private readonly camera = new PerspectiveCamera(42, 1, 0.01, 1000);
  private readonly scene = new Scene();
  private readonly controls: OrbitControls;
  private readonly observer: ResizeObserver;
  private readonly environment;
  private readonly grid = new GridHelper(20, 40, 0x435160, 0x26313b);
  private view: MeshView | undefined;
  private reducedView: MeshView | undefined;
  private detailDistance = Infinity;
  private wireframe = false;
  private normals = false;
  private disposed = false;

  constructor(canvas: HTMLCanvasElement, container: HTMLElement) {
    this.renderer = new WebGLRenderer({ canvas, antialias: true });
    this.renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
    this.renderer.toneMapping = ACESFilmicToneMapping;
    this.renderer.toneMappingExposure = 1.2;
    this.scene.background = new Color(0x151d25);
    const pmrem = new PMREMGenerator(this.renderer);
    const room = new RoomEnvironment();
    this.environment = pmrem.fromScene(room, 0.04);
    this.scene.environment = this.environment.texture;
    room.dispose(); pmrem.dispose();
    this.scene.add(new HemisphereLight(0xd5e6ff, 0x6f5542, 2));
    const key = new DirectionalLight(0xffecd5, 4);
    key.position.set(4, 7, 5); this.scene.add(key);
    this.scene.add(this.grid);
    this.camera.position.set(4, 3, 5);
    this.controls = new OrbitControls(this.camera, canvas);
    this.controls.enableDamping = true;
    this.controls.dampingFactor = 0.08;
    this.controls.target.set(0, 0.5, 0);
    const resize = () => {
      const width = Math.max(container.clientWidth, 1), height = Math.max(container.clientHeight, 1);
      this.renderer.setSize(width, height, false);
      this.camera.aspect = width / height; this.camera.updateProjectionMatrix();
    };
    this.observer = new ResizeObserver(resize); this.observer.observe(container); resize();
    this.renderer.setAnimationLoop(() => {
      this.controls.update(); this.updateDetail(); this.renderer.render(this.scene, this.camera);
    });
    canvas.addEventListener('webglcontextlost', this.contextLost);
    canvas.addEventListener('webglcontextrestored', this.contextRestored);
  }

  private contextLost = (event: Event) => {
    event.preventDefault();
    const status = document.getElementById('status')!;
    status.textContent = 'Graphics context lost. Waiting for restoration…'; status.dataset.kind = 'error';
  };
  private contextRestored = () => {
    document.getElementById('status')!.textContent = 'Graphics context restored.';
  };

  show(result: EvaluationResult, frame: boolean): { uploadMs: number; triangles: number } {
    const start = performance.now();
    const next = createMeshView(result);
    next.setDisplay(this.wireframe, this.normals);
    const level = result.lods[0];
    let reduced: MeshView | undefined;
    try {
      if (level) reduced = createMeshView({ ...result, ...level });
      reduced?.setDisplay(this.wireframe, this.normals);
    } catch (error) { next.dispose(); throw error; }
    this.clear(); this.view = next; this.reducedView = reduced; this.scene.add(next.root);
    if (reduced) { reduced.root.visible = false; this.scene.add(reduced.root); }
    const bounds = new Box3().setFromObject(next.root);
    const radius = bounds.getSize(new Vector3()).length() / 2;
    this.detailDistance = radius / Math.sin(MathUtils.degToRad(this.camera.fov / 2)) * 2.5;
    if (frame) this.frame();
    // Include actual buffer upload rather than only BufferGeometry construction.
    this.renderer.render(this.scene, this.camera);
    return { uploadMs: performance.now() - start, triangles: next.triangles };
  }

  setDisplay(wireframe: boolean, normals: boolean): void {
    this.wireframe = wireframe; this.normals = normals; this.view?.setDisplay(wireframe, normals);
    this.reducedView?.setDisplay(wireframe, normals);
  }

  private updateDetail(): void {
    if (!this.view) return;
    const reduced = !!this.reducedView && this.camera.position.distanceTo(this.controls.target) > this.detailDistance;
    this.view.root.visible = !reduced;
    if (this.reducedView) this.reducedView.root.visible = reduced;
    this.renderer.domElement.dataset.detail = reduced ? 'reduced' : 'full';
    this.renderer.domElement.dataset.renderedTriangles = String(reduced ? this.reducedView!.triangles : this.view.triangles);
  }

  clear(): void {
    this.view?.dispose(); this.reducedView?.dispose();
    this.view = undefined; this.reducedView = undefined;
    this.renderer.domElement.dataset.renderedTriangles = '0';
  }

  frame(): void {
    if (!this.view || this.view.root.children.length === 0) return;
    const bounds = new Box3().setFromObject(this.view.root);
    if (bounds.isEmpty()) return;
    const center = bounds.getCenter(new Vector3()), size = bounds.getSize(new Vector3());
    const radius = Math.max(size.length() / 2, 0.05);
    const halfFov = Math.min(MathUtils.degToRad(this.camera.fov / 2), Math.atan(Math.tan(MathUtils.degToRad(this.camera.fov / 2)) * this.camera.aspect));
    const distance = radius / Math.sin(halfFov) * 1.15;
    this.controls.target.copy(center);
    this.camera.position.copy(center).add(new Vector3(1, 0.65, 1.25).normalize().multiplyScalar(distance));
    this.camera.near = Math.max(radius / 1000, 0.001); this.camera.far = distance + radius * 100;
    this.camera.updateProjectionMatrix();
    this.controls.maxDistance = distance * 20;
    this.controls.update();
    this.grid.position.y = bounds.min.y - radius * 0.005;
    this.grid.scale.setScalar(Math.max(radius / 4, 0.05));
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true; this.renderer.setAnimationLoop(null); this.observer.disconnect();
    this.controls.dispose(); this.clear(); this.environment.dispose();
    this.grid.geometry.dispose();
    const gridMaterials = Array.isArray(this.grid.material) ? this.grid.material : [this.grid.material];
    gridMaterials.forEach(material => material.dispose());
    this.renderer.domElement.removeEventListener('webglcontextlost', this.contextLost);
    this.renderer.domElement.removeEventListener('webglcontextrestored', this.contextRestored);
    this.renderer.dispose();
  }
}
