import { emitCone } from 'shared-lib/conifer';
class ConiferCone extends Part {
  static params = { species: 0, variant: 0 };
  static lods = [{ at: 0 }, { at: 3, gen: { gen: 'decimate', error: 0.0015 } },
    { at: 12, impostor: true }];
  build(p) { emitCone(this, p); }
}
