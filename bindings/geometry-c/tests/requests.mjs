// Build the original C consumer requests in memory, without SDK validation or
// JSON fixture reads. Native equivalence also checks these independent values.
const identity = () => [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];
const input = (reference, type = 'mesh', port = 'operand') => ({kind:'node',port,reference,type});
const node = (id, operation, parameters, materials = ['clay'], inputs = [], transform = identity(), output_type = 'mesh') =>
  ({id,operation,operation_version:1,output_type,parameters,inputs,transform,materials});
const request = (name, nodes, outputs) => ({binding_version:1,recipe:{
  asset_dependencies:[],canonical_profile:'matter-exact-json-v1',document_type:'recipe',extensions:[],
  materials:[{base_color:{factor:[0.75,0.25,0.125,1],texture:null,uv_set:0},emissive:[0,0,0],
    emissive_texture:null,id:'clay',metallic:0,metallic_roughness_texture:null,normal_scale:1,
    normal_texture:null,roughness:0.800000011920929}],metadata:{},nodes,outputs,
  parameters:{enabled:true,width:0.1},required_capabilities:[{name:'matter.recipe.core',version:1},{name:'matter.material.pbr',version:1}],
  resources:[],rng_algorithm:'matter.splitmix64.v1',schema_version:{major:1,minor:0},seed:'0123456789abcdef',
  source:{content:null,id:`binding.${name}`,legacy_hashes:[],revision:'1',uri:`fixture://binding/${name}`},source_map:{},textures:[]}});

export function makeRequests() {
  const box = () => node('box','primitive.box',{size:[2,4,6]});
  const sphere = () => node('sphere','primitive.sphere',{radius:1,segments:8});
  const requests = {
    box:request('box',[box()],['box']),
    extrude:request('extrude',[
      node('profile','profile.polygon',{points:[[0,0],[1,0],[1,1],[0,1]]},[],[],identity(),'profile'),
      node('extrude','profile.extrude',{depth:2},['clay'],[input('profile','profile')])],['extrude']),
    sphere:request('sphere',[sphere()],['sphere']),
    placement:request('placement',[box(),
      node('left','placement.affine',{},[],[input('box')],[1,0,0,-3,0,1,0,0,0,0,1,0,0,0,0,1]),
      node('right','placement.affine',{},[],[input('box')],[-2,0,0,3,0,3,0,0,0,0,4,0,0,0,0,1])],['left','right']),
    csg:request('csg',[sphere(),node('cutter','primitive.box',{size:[1,1,1]}),
      node('cutter.placed','placement.affine',{},[],[input('cutter','mesh','mesh')],[1,0,0,0.5,0,1,0,0,0,0,1,0,0,0,0,1]),
      node('cut','solid.ordered',{operations:['union','subtract'],spacing:0.25,smoothing:0},['clay'],
        [input('sphere'),input('cutter.placed')])],['cut']),
    process:{binding_version:1,required_capabilities:[{name:'matter.operation.mesh.simplify',version:1},{name:'matter.operation.mesh.smooth',version:1}],
      settings:{regions:[{prototypes:['sphere'],modifiers:[{operation:'mesh.simplify',ratio:0.6},{operation:'mesh.smooth',iterations:2,lambda:0.5,mu:-0.53}]}]}},
    query:{binding_version:1,samples:[{prototype:'box',kind:'ray_surface',position:[0,10,0],direction:[0,-2,0],max_distance_metres:20},
      {prototype:'box',kind:'closest_surface',position:[10,0,0],max_distance_metres:1}]}
  };
  requests.csg.recipe.required_capabilities.push({name:'matter.operation.solid.ordered',version:1});
  return requests;
}
