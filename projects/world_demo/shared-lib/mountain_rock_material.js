// Continuous local mineral structure. Geometry owns the large fractures;
// this source adds weathered grain, shallow fissures and pitted microrelief.
// All channels share the same features and fade with their texel footprint.
import { checkedMountainRockSize } from 'shared-lib/mountain_rock_sizes';

export function mountainRockMaterial(s,{shape=0,seed=0,referenceSizeM=1}={}) {
  checkedMountainRockSize(referenceSizeM);
  const key=28411+shape*1789+seed*191;
  // Broad mineral variation follows the whole rock; all smaller features and
  // physical height below retain the same metre dimensions in every class.
  const mineral=s.noise3(key,2.3/referenceSizeM,3,.5,2,
    {seed:key+1,freq:3.7/referenceSizeM,amp:.08*referenceSizeM});
  const weather=s.noise3(key+17,7,2);
  const mediumFade=s.footprint.smoothstep(.015,.08).oneMinus();
  const fineFade=s.footprint.smoothstep(.002,.012).oneMinus();
  const body=weather.mul(mediumFade);
  const grain=s.noise3(key+31,80,2).mul(fineFade);
  // Pits must span several production texels (about 5–6 mm each). Filtering
  // them with the sub-centimetre grain erased their height before POM saw it.
  const pits=s.noise3(key+53,21,2).smoothstep(.15,.50).mul(mediumFade);
  const fissures=s.noise3(key+73,12,2).abs().smoothstep(.025,.11).oneMinus()
    .mul(weather.smoothstep(.15,.5)).mul(mediumFade);
  const value=mineral.mul(.030).add(body.mul(.012)).add(grain.mul(.007))
    .sub(pits.mul(.015)).sub(fissures.mul(.008)).add(.135);
  const warmth=mineral.smoothstep(.0,.45).mul(.013);
  return {
    baseColor:[value.add(warmth),value.mul(.97),value.mul(.89).sub(warmth.mul(.5))],
    roughness:body.mul(.045).add(pits.mul(.05)).add(.86),
    occlusion:pits.mul(-.12).sub(fissures.mul(.08)).add(1),
    height:body.mul(.0025).add(grain.mul(.00025)).sub(pits.mul(.006))
      .sub(fissures.mul(.0035)).sub(.004),
    heightRange:[-.018,0],
  };
}
