import { emitBarkTexture } from 'shared-lib/conifer_bark';
class ConiferBranchDetail extends Tileset {
  static requires = [{ module: 'ConiferBarkRelief', params: { kind: 'branch' } }];
  build() { emitBarkTexture(this, 'branch', MAT.bark); }
}
