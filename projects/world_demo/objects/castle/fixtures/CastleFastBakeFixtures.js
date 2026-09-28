// Scene-local foundation, closed glazing/gold frame and source-brick plinth.
class CastleFastBakeFixtures extends Part {
  static lodBudgets=[1];
  static noImpostor=true;
  static params={foundation:MAT.stone,stone:MAT.stone,gold:MAT.metal,glass:MAT.glass};
  build(p){
    this.fill(p.foundation);this.box([0,-.28,0],[9,.12,7]);
    this.fill(p.stone);this.box([4.8,.45,4.8],[.4,.45,.4]);
    // Entirely inside the1x1.2m window void; the adjacent doorway stays open.
    this.fill(p.glass);this.box([1,1.6,-3.98],[.40,.50,.015]);
    this.fill(p.gold);
    for(const x of [.57,1.43])this.box([x,1.6,-3.96],[.025,.55,.025]);
    for(const y of [1.075,2.125])this.box([1,y,-3.96],[.455,.025,.025]);
  }
}
