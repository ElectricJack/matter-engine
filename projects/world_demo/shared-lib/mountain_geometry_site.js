// Fixed, deterministic detailed-rock inspection site. Reuse a bounded set of
// dense source assets; placements keep their world coordinates across sector
// splits and merges.
//
// The default is the three focal rocks of the 2026-09-18 mountain pilot, at the
// current resolution 128. `{stress:true}` adds the 2026-09-19 stress grid
// (1,280 more rocks, 16 more assets) for explicit high-density benchmarks only;
// its records are unchanged so that evidence stays reproducible.
const hash = (x,z,s) => {
  let h=Math.imul(x+101,374761393)^Math.imul(z+73,668265263)^s;
  h=Math.imul(h^(h>>>13),1274126177);return (h^(h>>>16))>>>0;
};
export const mountainGeometrySamples = (material,{stress=false}={}) => {
  const samples=[
    {x:416,z:1456,params:{seed:0,shape:0,size:6,resolution:128,material}},
    {x:425,z:1458,params:{seed:1,shape:2,size:4,resolution:128,material}},
    {x:422,z:1448,params:{seed:2,shape:1,size:7,resolution:128,material}},
  ];
  if(!stress)return samples;
  const variants=Array.from({length:16},(_,i)=>({
    seed:100+i,shape:i%3,size:i<8?3+(i%4)*1.5:.25+(i%8)*.25,
    resolution:128,material,
  }));
  for(let z=0;z<32;z++)for(let x=0;x<32;x++) {
    const h=hash(x,z,0x71a3);
    samples.push({x:300+x*8+((h&255)/255-.5)*5,
      z:1320+z*8+(((h>>>8)&255)/255-.5)*5,
      params:variants[8+((h>>>16)%8)]});
    if((x&1)===0&&(z&1)===0) {
      const b=hash(x,z,0x4b21);
      samples.push({x:300+x*8+3,z:1320+z*8+3,params:variants[b%8]});
    }
  }
  return samples;
};
export const mountainGeometryCatalog = (material,options) => {
  const unique=new Map();
  for(const sample of mountainGeometrySamples(material,options))
    unique.set(JSON.stringify(sample.params),sample.params);
  return [...unique.values()].map(params=>({module:'MountainDetailRock',params}));
};
