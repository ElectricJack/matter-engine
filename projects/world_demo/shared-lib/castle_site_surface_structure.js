// The existing castle structure layout owns topology, openings and joinery.
// This adapter changes only its geometry backend: exact physical dimensions,
// planar bevels and polygonal hardware, with no voxel work or instance scaling.
import { structureLayout, structureRecipes, structureMaterialParams, applyFrame } from 'shared-lib/castle_structure';
import { makeSurfaceFace, emitSurfaceShell } from 'shared-lib/castle_surface_shells';

const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const add=(a,b)=>a.map((v,i)=>v+b[i]);
const mul=(a,s)=>a.map(v=>v*s);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const unit=a=>mul(a,1/Math.hypot(...a));
const keyToParam=key=>'mat'+key[0].toUpperCase()+key.slice(1);
const defaults=structureMaterialParams();
function material(params,key){
 const name=keyToParam(key),value=params[name]??defaults[name];
 if(!Number.isInteger(value)||value<0)throw new TypeError('Invalid structure material '+key);
 return value;
}

// Quote-free scalar transport is deliberate: the native PartGraph v1 params
// serializer does not escape string values. URI encoding also excludes its
// memo-key separators, so a record remains one unambiguous scalar identity.
const RECORD_PAYLOAD_SCHEMA='matter.castle-surface-structure-record/v1';
export const SITE_STRUCTURE_PAYLOAD_LIMIT=8*1024*1024;
const stairStyleCode=value=>value===1||value==='stone'?1:value===2||value==='timber'?2:0;
export function siteSurfaceStructureRecipes(manifest,options={}){
 return structureRecipes(manifest,{module:'CastleWingSurfaceStructure',split:false,...options}).map(recipe=>{
  const record=siteSurfaceStructureRecord(manifest,recipe.params);
  const recordPayload=encodeURIComponent(JSON.stringify({schema:RECORD_PAYLOAD_SCHEMA,
   manifestId:manifest.planId,stairStyle:stairStyleCode(recipe.params.stairStyle),record}));
  if(recordPayload.length>SITE_STRUCTURE_PAYLOAD_LIMIT)throw new RangeError('Structure record payload exceeds transport budget');
  return {...recipe,params:{...recipe.params,recordPayload}};
 });
}
function validatePayloadOps(ops){
 const vec=value=>Array.isArray(value)&&value.length===3&&value.every(Number.isFinite);
 const positive=value=>Number.isFinite(value)&&value>0;
 const mat=value=>typeof value==='string'&&Object.hasOwn(defaults,keyToParam(value));
 if(ops.length>20000)throw new RangeError('Structure record exceeds operation budget');
 for(const op of ops){
  if(!op||typeof op!=='object')throw new Error('Invalid structure payload operation');
  const f=op.frame;
  if(f!==undefined&&f!==null&&(typeof f!=='object'||Array.isArray(f)||!vec(f.t??[0,0,0])||['rx','ry','rz'].some(k=>f[k]!==undefined&&!Number.isFinite(f[k]))))
   throw new Error('Invalid structure payload frame');
  let valid=false;
  if(op.op==='box')valid=mat(op.material)&&vec(op.center)&&vec(op.half)&&op.half.every(positive);
  else if(op.op==='cyl')valid=mat(op.material)&&vec(op.a)&&vec(op.b)&&positive(op.r)&&Math.hypot(...sub(op.a,op.b))>0;
  else if(op.op==='tris')valid=mat(op.material)&&Array.isArray(op.verts)&&op.verts.length%9===0&&op.verts.every(Number.isFinite);
  else if(op.op==='child'){
   const p=op.params;
   valid=['CastleBeam','CastlePlank','CastleStone'].includes(op.module)&&p&&mat(p.material)&&
    positive(p.length)&&positive(p.height??p.thickness)&&positive(p.depth??p.width)&&
    (p.endMaterial===undefined||mat(p.endMaterial))&&(p.ironMaterial===undefined||mat(p.ironMaterial));
  }
  if(!valid)throw new Error('Invalid structure payload '+op.op+' operation');
 }
}
function payloadRecord(params){
 if(typeof params.recordPayload!=='string')throw new TypeError('Structure recordPayload must be a string');
 if(params.recordPayload.length>SITE_STRUCTURE_PAYLOAD_LIMIT)throw new RangeError('Structure record payload exceeds transport budget');
 if(/[^A-Za-z0-9_.!~*'()%\-]/.test(params.recordPayload))throw new Error('Structure record payload must be URI encoded');
 const payload=JSON.parse(decodeURIComponent(params.recordPayload)),record=payload?.record;
 if(payload?.schema!==RECORD_PAYLOAD_SCHEMA)throw new Error('Unsupported structure record payload schema');
 if(typeof payload.manifestId!=='string'||!payload.manifestId||!record||typeof record.id!=='string'||
    !['floor','stair','roof','frame'].includes(record.kind)||!Number.isInteger(record.index)||!Array.isArray(record.ops)||
    !Array.isArray(record.anchor)||record.anchor.length!==3||!record.anchor.every(Number.isFinite))
  throw new Error('Invalid structure record payload');
 if(payload.manifestId!==params.manifestId||record.id!==params.recordId||
    (params.recordKind!==undefined&&record.kind!==params.recordKind)||
    (params.recordIndex!==undefined&&record.index!==params.recordIndex)||
    payload.stairStyle!==stairStyleCode(params.stairStyle))
  throw new Error('Structure record payload identity mismatch');
 validatePayloadOps(record.ops);
 return record;
}
export function siteSurfaceStructureRecord(manifest,params={}){
 if(params.recordPayload!==undefined&&params.recordPayload!=='')return payloadRecord(params);
 if(params.manifestId&&params.manifestId!==manifest.planId)throw new Error('Structure manifest mismatch');
 const record=structureLayout(manifest,params).byId.get(params.recordId);
 if(!record)throw new Error('Unknown surface structure record '+params.recordId);
 if(params.recordKind&&params.recordKind!==record.kind)throw new Error('Structure record kind mismatch');
 return record;
}

// An eight-sided physical section extruded to its authored length. The bevel
// is cut into the section, never added outside the floor/clearance envelope.
// End caps have their own grain material and metric face frame.
export function buildSiteStructureBox(center,size,bevel=.004,timber=false){
 if(!size.every(v=>Number.isFinite(v)&&v>0))throw new RangeError('Invalid physical structure box');
 const [hx,hy,hz]=mul(size,.5),b=Math.min(bevel,hy*.25,hz*.25);
 const profile=[[hy,hz-b],[hy-b,hz],[-hy+b,hz],[-hy,hz-b],[-hy,-hz+b],[-hy+b,-hz],[hy-b,-hz],[hy,-hz+b]];
 const pt=(end,i)=>add(center,[end*hx,...profile[i%8]]),faces=[];
 for(const end of [-1,1])faces.push(makeSurfaceFace(profile.map((_,i)=>pt(end,i)),[end,0,0],`end-${end}`,timber?'endGrain':'body'));
 for(let i=0;i<8;i++){
  const a=profile[i],z=profile[(i+1)%8];
  faces.push(makeSurfaceFace([pt(-1,i),pt(1,i),pt(1,i+1),pt(-1,i+1)],[0,z[1]-a[1],a[0]-z[0]],`side-${i}`));
 }
 return {faces};
}
function cylinderShell(a,b,r){
 const axis=unit(sub(b,a)),u=unit(cross(axis,Math.abs(axis[1])<.9?[0,1,0]:[1,0,0])),v=cross(axis,u);
 const ring=center=>Array.from({length:8},(_,i)=>add(center,add(mul(u,r*Math.cos(i*Math.PI/4)),mul(v,r*Math.sin(i*Math.PI/4)))));
 const bottom=ring(a),top=ring(b),faces=[makeSurfaceFace(bottom,mul(axis,-1),'cap-a'),makeSurfaceFace(top,axis,'cap-b')];
 for(let i=0;i<8;i++){const j=(i+1)%8;faces.push(makeSurfaceFace([bottom[i],bottom[j],top[j],top[i]],sub(mul(add(bottom[i],bottom[j]),.5),a),'barrel-'+i));}
 return {faces};
}
// Compile once per operation, including all of its mortise/strap shells.
// applyFrame builds three rotation matrices; avoid that work per vertex.
function structureFrame(frame,anchor){
 const rotationFrame={...frame,t:[0,0,0]};
 const x=applyFrame(rotationFrame,[1,0,0]),y=applyFrame(rotationFrame,[0,1,0]),z=applyFrame(rotationFrame,[0,0,1]);
 const offset=sub(frame?.t??[0,0,0],anchor);
 const rotate=p=>[x[0]*p[0]+y[0]*p[1]+z[0]*p[2],x[1]*p[0]+y[1]*p[1]+z[1]*p[2],x[2]*p[0]+y[2]*p[1]+z[2]*p[2]];
 return {point:p=>add(rotate(p),offset),normal:rotate};
}
function transformed(shell,transform){
 return {faces:shell.faces.map(f=>({...f,positions:f.positions.map(transform.point),normal:transform.normal(f.normal)}))};
}
function childShells(op){
 const p=op.params,timber=op.module!=='CastleStone';
 if(!['CastleStone','CastleBeam','CastlePlank'].includes(op.module))throw new Error('Unsupported structure child '+op.module);
 const L=p.length,H=p.height??p.thickness,W=p.depth??p.width;
 const center=[0,timber?0:H*.5,0],out=[];
 const box=(c,s,region='body',wood=timber)=>out.push({shell:buildSiteStructureBox(c,s,wood?.004:.006,wood),region});
 // A real open end mortise, made from five closed physical pieces. Stepped
 // scarf ends retain a visible lap and shoulder without a CSG/voxel pass.
 if(timber&&p.joint===2){
  const cut=Math.min(L*.15,Math.min(H,W)*.32),x=L*.5-cut;
  box([-cut*.5,0,0],[L-cut,H,W]);
  for(const side of [-1,1]){
   box([x+cut*.5,side*H*.33,0],[cut,H*.34,W]);
   box([x+cut*.5,0,side*W*.33],[cut,H*.32,W*.34]);
  }
 }else if(timber&&p.joint===3){
  const cut=Math.min(L*.2,Math.min(H,W)*.68);
  box([0,0,0],[L-2*cut,H,W]);
  for(const side of [-1,1])box([side*(L*.5-cut*.5),-side*H*.25,0],[cut,H*.5,W]);
 }else box(center,[L,H,W]);
 if(timber&&(p.joint===1||p.joint===2)){
  const x=L*.5-Math.min(H,W)*.48,r=Math.max(.014,Math.min(H,W)*.075);
  out.push({shell:cylinderShell([x,0,-W*.5-.009],[x,0,W*.5+.009],r),region:'endGrain'});
 }
 if(timber&&p.strap){
  const width=Math.min(L*.08,Math.max(.04,Math.min(H,W)*.23));
  for(const side of [-1,1]){
   const x=side*(L*.5-Math.min(H,W)*.75);
   box([x,H*.5+.006,0],[width,.018,W], 'metal',false);
   for(const z of [-1,1])box([x,0,z*(W*.5+.006)],[width,H,.018],'metal',false);
  }
 }
 return out;
}

// A single furniture member in its local physical frame. Material handles
// are already resolved by the furnishing recipe; iron/end grain stay distinct.
export function emitSiteStructurePrimitive(part,module,params){
 const materials={body:params.material,endGrain:params.endMaterial??params.material,metal:params.ironMaterial??3};
 for(const item of childShells({module,params}))
  emitSurfaceShell(part,item.shell,item.region==='body'?materials:{body:materials[item.region],endGrain:materials[item.region],metal:materials[item.region]});
}

export function emitSiteSurfaceStructure(part,manifest,params={}){
 const record=siteSurfaceStructureRecord(manifest,params),layer=params.layer??0;
 // Bucket direct triangles by material once per record. This avoids creating
 // thousands of individual face shapes while retaining metric UV/end grain.
 const buckets=new Map();let active;
 const sink={fill(m){if(!buckets.has(m))buckets.set(m,[]);active=buckets.get(m);},beginShape(){},endShape(){},surfaceVertex(...v){active.push(v);}};
 let operations=0;
 for(const op of record.ops){
  if(op.op==='child'?layer===1:layer===2)continue;
  operations++;
  const transform=structureFrame(op.frame,record.anchor);
  const emit=(shell,mats)=>emitSurfaceShell(sink,transformed(shell,transform),mats);
  if(op.op==='child'){
   const mats={body:material(params,op.params.material),endGrain:material(params,op.params.endMaterial??op.params.material),metal:material(params,op.params.ironMaterial??'iron')};
   for(const item of childShells(op))emit(item.shell,item.region==='body'?mats:{body:mats[item.region],endGrain:mats[item.region],metal:mats[item.region]});
  }else if(op.op==='box')emit(buildSiteStructureBox(op.center,mul(op.half,2),.002,op.material==='oak'),{body:material(params,op.material),endGrain:material(params,op.material==='oak'?'oakEnd':op.material)});
  else if(op.op==='cyl')emit(cylinderShell(op.a,op.b,op.r),{body:material(params,op.material)});
  else if(op.op==='tris'){
   for(let i=0;i<op.verts.length;i+=9){
    const points=[op.verts.slice(i,i+3),op.verts.slice(i+3,i+6),op.verts.slice(i+6,i+9)],normal=cross(sub(points[1],points[0]),sub(points[2],points[0]));
    if(Math.hypot(...normal)<1e-12)continue;
    emit({faces:[makeSurfaceFace(points,normal,'structural-triangle')]},{body:material(params,op.material)});
   }
  }else throw new Error('Unsupported structure operation '+op.op);
 }
 let triangles=0;
 for(const [mat,vertices] of buckets){
  part.fill(mat);part.beginShape(0);
  for(const vertex of vertices){if(typeof part.surfaceVertex==='function')part.surfaceVertex(...vertex);else part.vertex(...vertex.slice(0,3));}
  part.endShape();triangles+=vertices.length/3;
 }
 return {record,operations,triangles,materialCount:buckets.size};
}
