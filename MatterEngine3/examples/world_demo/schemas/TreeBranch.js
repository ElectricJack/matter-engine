import { LSystem, LSystemFollower } from 'shared-lib/lsystem';

// Port of MatterEngine2's TreeBranch.cs, reworked into a bushier branch. An
// L-system follower walks a densely-branching skeleton; each segment is emitted
// as a tapered tube of mesh (via line()), and a "C" token drops a CLUSTER of
// leaves (a small fan) at that node. Clusters fire at many nodes, so each branch
// carries several groups of twigs and leaves instead of one terminal tuft.
// Baked once and instanced many times by Tree.
class TreeBranch extends Part {
  static requires = [{ module: 'Leaf' }];

  build(p) {
    const DEG = Math.PI / 180;
    const S = 0.5;                  // same world scale as Tree
    // Wide divergence angles so the branch fans out broadly instead of growing
    // as a tight, clumped broom.
    const a1 = -52 * DEG;
    const a2 =  60 * DEG;

    // No simplification on the twig tubes they are already as simple as possible
    //this.simplify(0.3);

    const lsys = new LSystem();
    lsys.init('X');
    // More bracketed splits per node => the branch forks repeatedly instead of
    // running out as one long whip. "C" marks a leaf-cluster node; it sits on
    // every sub-branch so foliage clusters cover the whole branch densely.
    lsys.rule('X => Fwd [Rotx1 Y C] [Rotx2 X C]');
    lsys.rule('Y => Fwd [Roty1 X C] [Roty2 Y C]');
    lsys.rule('Fwd => Fwd Fwd');
    lsys.rewrite(5);

    // Thicker tubes: a fatter start AND a much fatter tip (was 0.1*S) so the
    // twigs read as woody, not spindly hairs.
    const follower = new LSystemFollower(this, lsys, S, 1 * S, 13.0 * S);
    follower.addAction('Rotx1', () => this.rotateX(a1));
    follower.addAction('Roty1', () => this.rotateY(a1));
    //follower.addAction('Rotz1', () => this.rotateZ(a1));
    follower.addAction('Rotx2', () => this.rotateX(a2));
    follower.addAction('Roty2', () => this.rotateY(a2));
    //follower.addAction('Rotz2', () => this.rotateZ(a2));

    // Safety guards (v2 had none): a Rewrite(5) twig expands to tens of thousands
    // of tokens, so cap how much geometry we actually emit per branch.
    const MAX_LEAVES = 90;
    const MAX_SEGS   = 160;
    const CLUSTER    = 3;      // leaves per cluster (a light tuft, not a ball)
    let leaves = 0;

    // A leaf cluster: a tuft of CLUSTER blades splayed around the twig axis AND
    // varied in pitch (rotateX), so the tuft fills a little volume of foliage
    // instead of a flat ring. Each node sprouts a dense tuft.
    follower.addAction('C', () => {
      for (let i = 0; i < CLUSTER && leaves < MAX_LEAVES; ++i) {
        this.pushMatrix();
        this.rotateY(i * (360 / CLUSTER) * DEG + (Math.random() * 50 - 25) * DEG);
        this.rotateX(-40 * DEG - Math.random() * 70 * DEG);   // spread from out to up
        this.placeChild('Leaf');
        this.popMatrix();
        ++leaves;
      }
    });

    // Longer segments spread the leaf nodes apart, so the wide-angle fan reads as
    // an open, airy branch rather than a tight clump of overlapping tufts.
    follower.onGetForwardDistance = () => S;
    // follower.onEndForward = () => {
    //   this.rotateX((Math.random() * 8 - 4) * DEG);
    //   this.rotateZ((Math.random() * 6 - 3) * DEG);
    // };

    // Pass 1: record twig segments (absolute local-frame).
    const segs = [];
    follower.onSegment = (from, to, rFrom, rTo) => {
      if (segs.length < MAX_SEGS) segs.push([from, to, rFrom, rTo]);
    };
    follower.execute();

    // Pass 2: emit each segment as a tapered mesh tube. Stroke widths are
    // diameters, so the tube radius is half the width.
    this.fill(MAT.bark);
    for (const [from, to, wFrom, wTo] of segs) {
      this.line(from, to, wFrom * 0.5, wTo * 0.5);
    }
  }
}
