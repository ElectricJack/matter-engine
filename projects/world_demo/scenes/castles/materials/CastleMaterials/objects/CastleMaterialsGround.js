// Fixture-only neutral presentation slab. The reusable CastlePlank remains a
// detailed voxel primitive; using a direct box here avoids spending its detail
// budget on a nine-metre backdrop.
class CastleMaterialsGround extends Part {
  static params = { material: 8, backdropMaterial: 18 };

  build(p) {
    this.fill(p.material);
    this.box([0, -0.07, 0], [4.6, 0.07, 2.1]);
    this.fill(p.backdropMaterial);
    this.box([3.68, 1.35, 0.08], [1.1, 1.35, 0.06]);
  }
}
