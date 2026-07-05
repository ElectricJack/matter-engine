import { rng } from 'shared-lib/rng';
import { heightAt } from 'shared-lib/terrain_noise';

// Stage-4 GPU-culling stress fixture: COUNT children uniformly scattered over
// a 2 km x 2 km square, ground-following via the shared terrain noise. Kind is
// chosen deterministically by (i % 3):
//   bucket 0 -> Pebble          (scale 0.7 – 1.3)
//   bucket 1 -> Rock(seed=0)    (scale 1.2 – 2.4, "larger rocks")
//   bucket 2 -> Tree            (scale 0.9 – 1.1)
// The heterogeneous mix exists to exercise the per-part flatten decision:
// Tree flattens INLINE within itself, but this 50k-scatter parent must land on
// BOUNDARY so its .flat.part stores 50k FlatInstanceRefs instead of expanding.
// Same seeded rng drives every placement, so the scatter is deterministic and
// content-addressed (same contract as Meadow.js).
//
// NOTE: the scatter builder is INLINED in each StressForest<count> schema
// (rather than shared via `./stress_forest_lib`) because the module resolver
// only accepts 'shared-lib/...' specifiers — relative imports do not resolve
// (module_resolver.cpp resolve_specifier fails-closed on anything else).
// Keep the four copies in sync when editing.

const COUNT = 50000;
const SEED  = 20260703;
const W     = 2000.0;               // world span in x/z (2 km)

class StressForest50k extends Part {
  static requires = [
    { module: 'Pebble' },
    { module: 'Rock', params: { seed: 0 } },
    { module: 'Tree' },
  ];

  build(p) {
    const r = rng(SEED);
    for (let i = 0; i < COUNT; ++i) {
      const x = r.range(0, W), z = r.range(0, W);
      this.pushMatrix();
      this.translate(x, heightAt(x, z), z);
      this.rotateY(r.range(0, Math.PI * 2));
      const bucket = i % 3;
      if (bucket === 0) {
        const s = r.range(0.7, 1.3);
        this.scale(s, s, s);
        this.placeChild('Pebble');
      } else if (bucket === 1) {
        const s = r.range(1.2, 2.4);
        this.scale(s, s, s);
        this.placeChild('Rock', { seed: 0 });
      } else {
        const s = r.range(0.9, 1.1);
        this.scale(s, s, s);
        this.placeChild('Tree');
      }
      this.popMatrix();
    }
  }
}
