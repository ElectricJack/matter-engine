// Exactly these generators run in both native QuickJS and the portable JS SDK.
export function legacyProp({RecipeBuilder, legacyPart}, context) {
  const b = new RecipeBuilder(context); b.material('clay', {roughness: 0.8});
  const p = legacyPart(b, {prefix:'part', materialIds:{7:'clay'}, sourceLocation:() => ({uri:'legacy://prop',line:8,column:3})});
  p.fill(7); p.pushMatrix(); p.translate(2, 3, 4); p.scale(-2, 3, 4);
  p.box([1, 0, 0], [0.5, 1, 2]); p.popMatrix(); p.sphere([0, 0, 0], 1);
  return p.finish();
}
export function legacyCsg({RecipeBuilder, legacyPart}, context) {
  const b = new RecipeBuilder(context); b.material('clay');
  const p = legacyPart(b, {prefix:'solid', sourceLocation:() => ({uri:'legacy://csg',line:11,column:2})});
  p.fill('clay'); p.beginVoxels(0.25); p.box([0, 0, 0], [2, 3, 4]);
  p.sphere([1, 0, 0], 1); p.difference(); p.smoothing(0.125); p.endVoxels();
  return p.finish();
}
export function legacyChildren({RecipeBuilder, legacyPart}, context) {
  const b = new RecipeBuilder(context);
  const child = {id:'shared.child', content:{algorithm:'sha256',hex:'ab'.repeat(32)},schema_version:{major:1,minor:0}};
  const p = legacyPart(b, {resolveChild:() => child});
  p.placeChild('Pair', {width:2}); p.translate(3, 0, 0); p.placeChild('Pair', {width:2});
  return p.finish();
}
export function seeded({RecipeBuilder}, context) {
  const b = new RecipeBuilder(context), rng = b.rng();
  const words = [], units = [], unitRng = b.rng();
  for (let i=0;i<8;++i) {words.push(rng.nextU64().toString(16).padStart(16,'0')); units.push(unitRng.random());}
  const saved = rng.snapshot(), next = rng.nextU64();
  if (rng.restore(saved).nextU64() !== next || rng.clone().nextU64() !== rng.nextU64()) throw Error('RNG restore/clone mismatch');
  b.output(b.box('random', [1 + units[0], 1 + units[1], 1 + units[2]]));
  return {...b.build(), metadata:{words,units}};
}
export async function digest({RecipeBuilder}, context) {
  const b = new RecipeBuilder(context); b.output(b.box('digest', [0.1, 2, 3]));
  return {...b.build(), metadata:{digest:await b.digest()}};
}
