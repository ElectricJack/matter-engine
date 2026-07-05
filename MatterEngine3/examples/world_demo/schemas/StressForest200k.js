import { rng } from 'shared-lib/rng';
import { heightAt } from 'shared-lib/terrain_noise';

// Stage-4 GPU-culling stress fixture: COUNT pebbles uniformly scattered over a
// 2 km x 2 km square, ground-following via the shared terrain noise (no terrain
// tiles are placed — the field self-occludes, which is what HiZ measurement
// needs). One seeded rng drives every placement, so the scatter is
// deterministic and content-addressed (same contract as Meadow.js).
//
// NOTE: the scatter builder is INLINED in each StressForest<count> schema
// (rather than shared via `./stress_forest_lib`) because the module resolver
// only accepts 'shared-lib/...' specifiers — relative imports do not resolve
// (module_resolver.cpp resolve_specifier fails-closed on anything else).
// Keep the four copies in sync when editing.

const COUNT = 200000;
const SEED  = 20260703;
const W     = 2000.0;               // world span in x/z (2 km)

class StressForest200k extends Part {
  static requires = [{ module: 'Pebble' }];

  build(p) {
    const r = rng(SEED);
    for (let i = 0; i < COUNT; ++i) {
      const x = r.range(0, W), z = r.range(0, W);
      this.pushMatrix();
      this.translate(x, heightAt(x, z), z);
      this.rotateY(r.range(0, Math.PI * 2));
      const s = r.range(0.7, 1.3);
      this.scale(s, s, s);
      this.placeChild('Pebble');
      this.popMatrix();
    }
  }
}
