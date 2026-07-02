import { cubic } from 'shared-lib/bezier';

// A single flat leaf blade: two bezier-outlined halves emitted as TRIANGLE_FAN
// mesh (NOT voxels), ported from MatterEngine2's Leaf.cs. The blade lies in the
// X/Z plane and grows along +Z; Y stays ~0 so it reads as a thin surface.
class Leaf extends Part {
  build(p) {
    const LENGTH = 5.0;
    const cwNear = 8.0,  coNear = [0.1, -1.2];
    const cwFar  = 1.0,  coFar  = [-0.1, -1.2];
    const SCALE  = 0.21;   // 5 * 0.21 ~= 1.0 units long; small blades so tufts don't clump

    const lerp = (a, b, t) => [a[0]+(b[0]-a[0])*t, a[1]+(b[1]-a[1])*t, a[2]+(b[2]-a[2])*t];
    const sub  = (a, b)    => [a[0]-b[0], a[1]-b[1], a[2]-b[2]];

    const origin = [0, 0, 0];
    const end    = [0, 0, LENGTH];
    const mid0   = lerp(origin, end, 0.2);
    const mid1   = lerp(origin, end, 0.6666);

    const leftCp0  = sub(mid0, [-cwNear * 0.5 + coNear[0], coNear[1], 0]);
    const rightCp0 = sub(mid0, [ cwNear * 0.5 + coNear[0], coNear[1], 0]);
    const leftCp1  = sub(mid1, [-cwFar * 0.5 + coFar[0],  coFar[1],  0]);
    const rightCp1 = sub(mid1, [ cwFar * 0.5 + coFar[0],  coFar[1],  0]);

    const left = [], right = [];
    for (let i = 1; i <= 5; ++i) {
      const t = i / 5.0;
      left.push(cubic(origin, leftCp0, leftCp1, end, t));
      right.push(cubic(origin, rightCp0, rightCp1, end, t));
    }

    this.fill(MAT.leaf);
    this.pushMatrix();
    this.scale(SCALE, SCALE, SCALE);

    const v = (q) => this.vertex(q[0], q[1], q[2]);

    // Left half
    this.beginShape(SHAPE.fan);
      v(mid0); v(origin); v(left[0]); v(left[1]); v(left[2]); v(mid1);
    this.endShape();
    this.beginShape(SHAPE.fan);
      v(mid1); v(left[2]); v(left[3]); v(end);
    this.endShape();

    // Right half
    this.beginShape(SHAPE.fan);
      v(mid0); v(origin); v(right[0]); v(right[1]); v(right[2]); v(mid1);
    this.endShape();
    this.beginShape(SHAPE.fan);
      v(mid1); v(right[2]); v(right[3]); v(end);
    this.endShape();

    this.popMatrix();
  }
}
