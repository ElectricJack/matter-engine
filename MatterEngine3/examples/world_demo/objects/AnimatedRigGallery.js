// AnimatedRigGallery is deliberately a single deterministic bake source: it
// exercises the procedural-animation pipeline without loading meshes or clips
// from disk.  Its acceptance census is 21 joints, 3 rigid segments, 1
// attachment, 4 declared targets, and 5 graph nodes.
//
// The ground query is intentionally owned by the native proceduralGait
// controller. Runtime JavaScript is not used to drive the targets.
class AnimatedRigGallery extends Part {
  static requires = [{ module: 'Crate' }];
  // Keep a real two-rung authored ladder in the shipped acceptance asset.
  // Binding bake samples each finalized rung independently; runtime skinning
  // must therefore be able to select either stream rather than assuming LOD0.
  static lodBudgets = [1.0, 0.35];
  static lodAnchorSize = 1.0;

  build(p) {
    // Soft creature, mirrored limbs, a separate rigid segmented boom, and a
    // socket attachment all share one procedurally declared skeleton.
    this.beginRig('gallery');
    this.radius(0.32);
    this.root('root', [0, 1.2, 0]);
    this.bone('spine', [0, 1.15, 0]);
    this.radius(0.23);
    this.bone('neck', [0, 0.58, 0]);
    this.radius(0.35);
    this.bone('head', [0, 0.45, 0]);
    this.atJoint('spine');
    this.radius(0.20);
    this.bone('leftShoulder', [0.62, 0.33, 0]);
    this.bone('leftElbow', [0.72, -0.32, 0.10]);
    this.bone('leftHand', [0.58, -0.15, 0]);
    this.socket('leftGrip');
    this.atJoint('spine');
    this.mirrorBranch('leftShoulder', 'rightShoulder', {
      axis: 'x', rename: { from: 'left', to: 'right' }
    });
    this.atJoint('root');
    this.radius(0.25);
    this.bone('leftHip', [0.38, -0.20, 0]);
    this.bone('leftKnee', [0.05, -0.95, 0.14]);
    this.bone('leftFoot', [0.10, -0.82, 0.40]);
    this.atJoint('root');
    this.mirrorBranch('leftHip', 'rightHip', {
      axis: 'x', rename: { from: 'left', to: 'right' }
    });
    // Independent exact-three-joint look chain for an external graph target.
    this.atJoint('head');
    this.radius(0.10);
    this.bone('lookMid', [0, 0.12, 0.32]);
    this.bone('lookEnd', [0, 0.08, 0.30]);
    this.atJoint('root');
    this.radius(0.18);
    this.bone('machineBase', [-0.62, 0.28, 0]);
    this.bone('machineBoom', [-0.75, 0.48, 0]);
    this.bone('machineTool', [-0.62, 0.18, 0]);
    this.socket('toolSocket', { position: [-0.25, 0.0, 0.0] });
    this.endRig();

    // Generated tapered voxel fields are the creature's deformable mesh. The
    // binding recomputes weights after each LOD bake.
    this.skin('creatureSkin', {
      joints: ['spine', 'neck', 'head',
               'leftShoulder', 'leftElbow', 'leftHand',
               'rightShoulder', 'rightElbow', 'rightHand',
               'leftHip', 'leftKnee', 'leftFoot',
               'rightHip', 'rightKnee', 'rightFoot',
               'lookMid', 'lookEnd'],
      radiusScale: 1.15, falloffScale: 1.45, generate: false
    });
    this.bind('creatureSkin', () => {
      // Explicit procedural volumes keep the production preview inside the
      // ordinary Part bake path while the skeleton remains the weight source.
      this.beginVoxels(0.18);
      this.sphere([0, 1.65, 0], 0.72);
      this.sphere([0, 2.65, 0], 0.48);
      this.sphere([-0.72, 1.72, 0], 0.30);
      this.sphere([0.72, 1.72, 0], 0.30);
      this.sphere([-0.30, 0.55, 0.08], 0.34);
      this.sphere([0.30, 0.55, 0.08], 0.34);
      this.endVoxels();
    });
    // These segments stay rigid and travel through the dynamic raster/TLAS
    // transform lane; no deforming BLAS update is requested.
    this.segments('machineBaseSegment', { joints: ['machineBase'] });
    this.segments('machineBoomSegment', { joints: ['machineBoom'] });
    this.segments('machineToolSegment', { joints: ['machineTool'] });
    this.bind('machineBaseSegment', () => {
      // Each rigid owner receives its own authored triangles.  This is not a
      // draw-time reuse hint: the bake partitions all geometry exactly once.
      this.fill(MAT.stone);
      this.beginShape(SHAPE.triangles);
      this.vertex(-0.10, 0.00, -0.10); this.vertex(-0.72, 0.00, -0.10); this.vertex(-0.10, 0.18, -0.10);
      this.endShape();
    });
    this.bind('machineBoomSegment', () => {
      this.fill(MAT.stone);
      this.beginShape(SHAPE.triangles);
      this.vertex(-0.72, 0.00, -0.10); this.vertex(-1.35, 0.00, 0.10); this.vertex(-0.72, 0.18, -0.10);
      this.endShape();
    });
    this.bind('machineToolSegment', () => {
      this.fill(MAT.stone);
      this.beginShape(SHAPE.triangles);
      this.vertex(-1.35, 0.00, 0.10); this.vertex(-1.95, 0.00, 0.10); this.vertex(-1.35, 0.18, 0.10);
      this.endShape();
    });
    this.attach('toolAttachment', 'toolSocket', 'Crate', {
      position: [-0.35, 0, 0], scale: [0.34, 0.34, 0.34]
    });

    // Generated clips: idle is a stationary breathing pose; walk supplies
    // root translation for the fixed root-motion authority path.
    this.beginClip('idle', { duration: 1.5, sampleRate: 12, loop: true });
    this.generate(phase => {
      this.at('spine'); this.rotateX(Math.sin(phase * Math.PI * 2) * 0.08);
      this.at('leftElbow'); this.rotateZ(Math.sin(phase * Math.PI * 2) * 0.10);
      this.at('rightElbow'); this.rotateZ(-Math.sin(phase * Math.PI * 2) * 0.10);
    });
    this.marker(0.0, 'idleStart');
    this.endClip();

    this.beginClip('walk', { duration: 0.8, sampleRate: 16, loop: true });
    this.generate(phase => {
      const swing = Math.sin(phase * Math.PI * 2);
      this.at('root'); this.translate(phase * 0.8, 0, 0);
      this.at('leftHip'); this.rotateZ(swing * 0.42);
      this.at('rightHip'); this.rotateZ(-swing * 0.42);
      this.at('leftKnee'); this.rotateZ(Math.max(0, -swing) * 0.48);
      this.at('rightKnee'); this.rotateZ(Math.max(0, swing) * 0.48);
      this.at('machineBoom'); this.rotateY(swing * 0.18);
    });
    this.marker(0.0, 'leftStep');
    this.marker(0.4, 'rightStep');
    this.endClip();

    this.beginMotion('galleryMotion');
    this.input('speed', { type: 'float', cadence: 'fixed', default: 0 });
    this.controller('gait', 'proceduralGait', { cadence: 'fixed' });
    // The native controller owns the grounded feet. Hand and look targets are
    // declared graph inputs for the embedding application, not JS callbacks.
    this.target('leftFootTarget', { start: 'leftHip', end: 'leftFoot',
      driver: { controller: 'gait' }, cadence: 'fixed', pole: [0, 0, 1], soften: 0.65 });
    this.target('rightFootTarget', { start: 'rightHip', end: 'rightFoot',
      driver: { controller: 'gait' }, cadence: 'fixed', pole: [0, 0, 1], soften: 0.65 });
    this.target('leftHandTarget', { start: 'leftShoulder', end: 'leftHand',
      driver: 'external', cadence: 'frame', pole: [0, 0, 1], soften: 0.75 });
    this.target('lookTarget', { start: 'head', end: 'lookEnd',
      driver: 'external', cadence: 'frame', pole: [0, 1, 0], soften: 0.8 });
    this.clipNode('idleNode', 'idle');
    this.clipNode('walkNode', 'walk');
    this.blend1D('speedBlend', 'speed', [[0, 'idleNode'], [1, 'walkNode']]);
    this.nativeController('gaitNode', 'gait', 'speedBlend');
    this.output('out', 'gaitNode');
    this.endMotion();
  }
}
