import { emitBarkTexture } from 'shared-lib/conifer_bark';
class RedwoodBarkDetail extends Tileset {
  static requires = [{ module: 'ConiferBarkRelief', params: { kind: 'redwood' } }];
  build() { emitBarkTexture(this, 'redwood', MAT.bark); }
}
