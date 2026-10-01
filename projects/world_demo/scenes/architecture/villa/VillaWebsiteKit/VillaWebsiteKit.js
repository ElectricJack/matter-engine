const M={stone:defineMaterial('VillaKit.stone',{"albedo":[0.67,0.6,0.48],"roughness":0.83,"detail":"VillaKitStoneDetail","detailMode":"surface"}),
plaster:defineMaterial('VillaKit.plaster',{"albedo":[0.79,0.76,0.67],"roughness":0.94}),
clay:defineMaterial('VillaKit.clay',{"albedo":[0.49,0.2,0.105],"roughness":0.74}),
bronze:defineMaterial('VillaKit.bronze',{"albedo":[0.43,0.29,0.12],"roughness":0.33,"metallic":0.85}),
soil:defineMaterial('VillaKit.soil',{"albedo":[0.055,0.042,0.025],"roughness":1}),
leaf:defineMaterial('VillaKit.leaf',{"albedo":[0.19,0.25,0.105],"roughness":0.8}),
leafBack:defineMaterial('VillaKit.leafBack',{"albedo":[0.36,0.39,0.23],"roughness":0.9}),
bark:defineMaterial('VillaKit.bark',{"albedo":[0.2,0.15,0.09],"roughness":0.95}),
glaze:defineMaterial('VillaKit.glaze',{"albedo":[0.045,0.2,0.21],"roughness":0.26,"clearcoat":0.3,"clearcoatRoughness":0.18}),
darkstone:defineMaterial('VillaKit.darkstone',{"albedo":[0.29,0.3,0.265],"roughness":0.84}),
marble:defineMaterial('VillaKit.marble',{"albedo":[0.79,0.79,0.77],"roughness":0.26,"clearcoat":0.08,"clearcoatRoughness":0.15,"detail":"VillaKitMarbleDetail","detailMode":"surface"})};
class VillaWebsiteKit extends World {
 static roots=[{id:'wall-3m',module:'VillaWallPanel',params:{quality:0,...M},transform:[1,0,0,-10,0,1,0,0,0,0,1,0,0,0,0,1]},
{id:'wall-3m-doorway',module:'VillaDoorway',params:{quality:0,...M},transform:[1,0,0,-5,0,1,0,0,0,0,1,0,0,0,0,1]},
{id:'entablature-3m',module:'VillaEntablature',params:{quality:0,...M},transform:[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]},
{id:'floor-slab-3x3',module:'VillaPaving',params:{quality:0,...M},transform:[1,0,0,5,0,1,0,0,0,0,1,0,0,0,0,1]},
{id:'pool-basin-3x3',module:'VillaPoolBasin',params:{quality:0,...M},transform:[1,0,0,10,0,1,0,0,0,0,1,0,0,0,0,1]},
{id:'pool-edge-straight',module:'VillaPoolCoping',params:{quality:0,...M},transform:[1,0,0,-10,0,1,0,0,0,0,1,5,0,0,0,1]},
{id:'pool-edge-corner',module:'VillaPoolCorner',params:{quality:0,...M},transform:[1,0,0,-5,0,1,0,0,0,0,1,5,0,0,0,1]},
{id:'fountain-tiered',module:'VillaTieredFountain',params:{quality:0,...M},transform:[1,0,0,0,0,1,0,0,0,0,1,5,0,0,0,1]},
{id:'fountain-wall',module:'VillaWallFountain',params:{quality:0,...M},transform:[1,0,0,5,0,1,0,0,0,0,1,5,0,0,0,1]},
{id:'planter-square',module:'VillaPlanter',params:{quality:0,...M},transform:[1,0,0,10,0,1,0,0,0,0,1,5,0,0,0,1]},
{id:'stair-run-3m',module:'VillaStairs',params:{quality:0,...M},transform:[1,0,0,-10,0,1,0,0,0,0,1,10,0,0,0,1]},
{id:'urn-small',module:'VillaAmphora',params:{quality:0,...M},transform:[1,0,0,-5,0,1,0,0,0,0,1,10,0,0,0,1]},
{id:'statue-a',module:'VillaSeedSculpture',params:{quality:0,...M},transform:[1,0,0,0,0,1,0,0,0,0,1,10,0,0,0,1]},
{id:'bench-3m',module:'VillaBench',params:{quality:0,...M},transform:[1,0,0,5,0,1,0,0,0,0,1,10,0,0,0,1]},
{id:'statue-b',module:'VillaStrataSculpture',params:{quality:0,...M},transform:[1,0,0,10,0,1,0,0,0,0,1,10,0,0,0,1]},
{id:'relief-a',module:'VillaNetworkRelief',params:{quality:0,...M},transform:[1,0,0,-10,0,1,0,0,0,0,1,15,0,0,0,1]},
{id:'urn-large',module:'VillaMosaicVessel',params:{quality:0,...M},transform:[1,0,0,-5,0,1,0,0,0,0,1,15,0,0,0,1]},
{id:'olive-small',module:'VillaOlive',params:{quality:0,...M},transform:[1,0,0,0,0,1,0,0,0,0,1,15,0,0,0,1]},
{id:'ground-plant-clump',module:'VillaHerbs',params:{quality:0,...M},transform:[1,0,0,5,0,1,0,0,0,0,1,15,0,0,0,1]},
{id:'wall-inset-panel',module:'VillaWallMedallion',params:{quality:0,...M},transform:[1,0,0,10,0,1,0,0,0,0,1,15,0,0,0,1]}];
 static camera={position:[18,13,-20],target:[0,1.5,7]};
 static atmosphere={groundAlbedo:.6};
 static lights={sun:{dir:[-.5,-.8,.3],color:[1,.94,.82]},sky:{color:[.7,.75,.85]}};
}
