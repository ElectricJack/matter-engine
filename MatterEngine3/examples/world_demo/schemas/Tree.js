import { expand } from 'shared-lib/lsystem';

class Tree extends Part {
  static requires = [{ module: 'Leaf' }];

  build(p) {
    const MAX_LEAVES = 400;
    let leaves = 0;

    // L-system: F = grow a segment, [ ] = branch push/pop, +-&^ = turns.
    const rules = {
      F: [
        { to: 'FF[+F&F][-F^F][&F+F]', weight: 3 },
        { to: 'FF[-F^F][+F&F]',       weight: 2 },
      ],
    };
    const seed = 1337;
    const sys = expand('F', rules, 4, seed);

    const STEP = 0.55;
    const TURN = 22 * Math.PI / 180;
    let depth = 0;

    this.beginVoxels(0.12);

    const branchRadius = (d) => Math.max(0.06, 0.42 * Math.pow(0.78, d));

    const segment = (d) => {
      this.fill(MAT.bark);
      const r0 = branchRadius(d);
      const r1 = branchRadius(d + 1);
      const N = 4;
      for (let i = 0; i <= N; ++i) {
        const t = i / N;
        const y = STEP * t;
        const r = r0 + (r1 - r0) * t;
        this.pushMatrix();
        this.translate(0, y, 0);
        this.sphere([0, 0, 0], r);
        this.popMatrix();
      }
      this.translate(0, STEP, 0);   // advance the turtle to the segment end
    };

    const placeLeaf = () => {
      if (leaves >= MAX_LEAVES) return;
      this.pushMatrix();
      this.rotateY(Math.random() * Math.PI * 2);
      this.rotateX((Math.random() - 0.5) * 0.8);
      this.translate(0, 0.15, 0);
      this.placeChild('Leaf');
      this.popMatrix();
      ++leaves;
    };

    for (let i = 0; i < sys.length; ++i) {
      const ch = sys[i];
      if (ch === 'F') {
        segment(depth);
        const next = sys[i + 1];
        if (next === ']' || next === undefined) placeLeaf();
      } else if (ch === '[') {
        this.pushMatrix(); ++depth;
      } else if (ch === ']') {
        this.popMatrix(); --depth;
      } else if (ch === '+') {
        this.rotateZ(TURN);
      } else if (ch === '-') {
        this.rotateZ(-TURN);
      } else if (ch === '&') {
        this.rotateX(TURN);
      } else if (ch === '^') {
        this.rotateX(-TURN);
      }
    }

    this.endVoxels();
  }
}
