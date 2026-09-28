import { emitBarkTexture } from 'shared-lib/conifer_bark';
class ConiferBarkDetail extends Tileset {
  static requires = [{ module: 'ConiferBarkRelief', params: { kind: 'trunk' } }];
  build() { emitBarkTexture(this, 'trunk', MAT.bark); }
}
