// Local metre-grid architecture. Placement/yaw and inter-wing vestibules belong
// to the site compiler; every program here remains internally traversable when
// unused exterior sockets are sealed. All dimensions are metres.
export const CASTLE_WING_DEFAULTS = Object.freeze({
  keep: Object.freeze({ width:12, depth:12, storeys:3 }),
  hall: Object.freeze({ width:14, depth:8, storeys:2 }),
  chapel: Object.freeze({ width:6, depth:13, storeys:2 }),
  service: Object.freeze({ width:10, depth:6, storeys:1 }),
});
const rect = (x,z,width,depth) => ({x,z,width,depth});
const levelId = n => n === 0 ? 'ground' : n === 1 ? 'upper' : `level-${n}`;
const roomId = (n,name) => `${levelId(n)}-${name}`;
const socketId = (n,side) => `${n===0?'':n===1?'upper-':`level-${n}-`}${side}-entry`;
function room(n,name,label,use,bounds,extra={}) {
  return {id:roomId(n,name),label,use,rect:bounds,floorType:n?'oak':'flags',
    ...(n?{floorStructure:{joistDirection:bounds.width<=bounds.depth?'x':'z',joistSpacing:.6}}:{}),...extra};
}
function edge(level,id,from,to,rooms,{open=false,rail=false,window=false}={}) {
  let length=Math.hypot(to[0]-from[0],to[1]-from[1]);
  // A 2m doorway uses adjacent ordinary wall edges for its jambs; a 3m
  // half-grid-centred bay retains .5m jambs within both end modules.
  if(!open&&!window&&length>=4) {
    const axis=from[0]===to[0]?1:0,center=(from[axis]+to[axis])/2;
    from=[...from];to=[...to];from[axis]=center-1;to[axis]=center+1;length=2;
  }
  const width=open?length:window?1.4:2;
  level.edgeOverrides.push({id,from,to,kind:open?'open':window?'window':'arch',
    ...(rooms?{connects:rooms}:{}),...(rail?{railProfile:'castle.oak-guardrail'}:{}),
    opening:{offset:(length-width)/2,width,height:open?3.5:window?1.9:2.8,bottom:window?1.1:0}});
}
function connect(level,name,from,to,a,b,open=false) {
  edge(level,`${level.id}-${name}`,from,to,[`${level.id}-${a}`,`${level.id}-${b}`],{open});
}
function socket(plan,n,side,from,to,name) {
  const level=plan.levels[n],id=socketId(n,side);
  edge(level,id,from,to,[roomId(n,name),'outside']);
  plan.wingProgram.sockets.push({sourceId:id,levelId:level.id,roomId:roomId(n,name),side,
    center:[(from[0]+to[0])/2,4*n,(from[1]+to[1])/2],from:[...level.edgeOverrides[level.edgeOverrides.length-1].from],to:[...level.edgeOverrides[level.edgeOverrides.length-1].to],width:2,height:2.8,
    outward:side==='north'?[0,0,1]:side==='south'?[0,0,-1]:side==='east'?[1,0,0]:[-1,0,0]});
}
function uStair(plan,n,x) {
  plan.stairs.push({id:`stair-${n}`,lowerLevelId:levelId(n),upperLevelId:levelId(n+1),
    lowerRoomId:roomId(n,'stair'),upperRoomId:roomId(n+1,'stair'),width:1.5,tread:.25,maxRiser:.2,headroom:2.2,
    entryDirection:'N',exitDirection:'S',flights:[
      {id:'out',footprint:rect(x+.5,3,1.5,2.5),direction:'N',stepCount:10},
      {id:'return',footprint:rect(x+2,3,1.5,2.5),direction:'S',stepCount:10}],landings:[
      {id:'lower',kind:'lower',bounds:rect(x+.5,1.5,1.5,1.5)},
      {id:'turn',kind:'intermediate',elevation:n*4+2,bounds:rect(x+.5,5.5,3,1.5)},
      {id:'upper',kind:'upper',bounds:rect(x+2,1.5,1.5,1.5)}]});
}
function voidRoom(plan,n,name,bounds,lowerName) {
  plan.levels[n].rooms.push(room(n,name,'Open to below','void',bounds,
    {required:false,openToBelow:true,lowerRoomId:roomId(n-1,lowerName),floorType:null,floorStructure:undefined}));
  plan.verticalVoids.push({id:name,kind:'double-height',lowerLevelId:levelId(n-1),upperLevelId:levelId(n),
    footprint:{...bounds},roomIds:[roomId(n-1,lowerName)]});
}
function keep(plan,count) {
  for(let n=0;n<count;n++) {
    const l=plan.levels[n];
    l.rooms.push(room(n,'main',n===0?'Guard hall':n===1?'Solar':'Council chamber',n===0?'hall':'chamber',rect(0,0,8,8)),
      room(n,'stair','Return stair vestibule','stair',rect(8,0,4,8)),
      room(n,'north',n===0?'Refectory':n===1?'Household chamber':'Library','chamber',rect(0,8,12,4)));
    connect(l,'main-stair',[8,0],[8,3],'main','stair');
    connect(l,'main-north',[2,8],[6,8],'main','north');
    socket(plan,n,'south',[4,0],[8,0],'main'); socket(plan,n,'north',[4,12],[8,12],'north');
    socket(plan,n,'west',[0,2],[0,6],'main'); socket(plan,n,'east',[12,0],[12,3],'stair');
    if(n<count-1)uStair(plan,n,8);
  }
}
function hall(plan) {
  const g=plan.levels[0],u=plan.levels[1];
  g.rooms.push(room(0,'hall','Great hall','hall',rect(0,0,10,8)),room(0,'stair','Hall stair','stair',rect(10,0,4,8)));
  connect(g,'hall-stair',[10,0],[10,3],'hall','stair');
  u.rooms.push(room(1,'west-gallery','Minstrels gallery','gallery',rect(0,0,2,6)),
    room(1,'south-gallery','South gallery','gallery',rect(2,0,8,2)),
    room(1,'north-gallery','North gallery','gallery',rect(0,6,10,2)),room(1,'stair','Hall landing','stair',rect(10,0,4,8)));
  voidRoom(plan,1,'hall-air',rect(2,2,8,4),'hall');
  connect(u,'gallery-stair',[10,0],[10,2],'south-gallery','stair',true);
  connect(u,'west-south',[2,0],[2,2],'west-gallery','south-gallery',true);
  connect(u,'west-north',[0,6],[2,6],'west-gallery','north-gallery',true);
  for(const [name,a,b] of [['west',[2,2],[2,6]],['south',[2,2],[10,2]],['north',[2,6],[10,6]]])
    edge(u,`gallery-rail-${name}`,a,b,null,{open:true,rail:true});
  for(let n=0;n<2;n++) {
    socket(plan,n,'south',[3,0],[7,0],n?'south-gallery':'hall');
    socket(plan,n,'north',[3,8],[7,8],n?'north-gallery':'hall');
    socket(plan,n,'west',[0,2],[0,6],n?'west-gallery':'hall');
    socket(plan,n,'east',[14,0],[14,3],'stair');
  }
  uStair(plan,0,10);
}
function chapel(plan) {
  const g=plan.levels[0],u=plan.levels[1];
  g.rooms.push(room(0,'nave','Chapel nave','chapel',rect(2,0,4,9)),room(0,'stair','Choir stair','stair',rect(0,0,2,9)),
    room(0,'chancel','Chancel','chapel',rect(0,9,6,4)));
  connect(g,'nave-stair',[2,0],[2,3],'nave','stair'); connect(g,'nave-chancel',[2,9],[6,9],'nave','chancel');
  u.rooms.push(room(1,'stair','Choir landing','stair',rect(0,0,2,9)),room(1,'choir','Organ and choir loft','gallery',rect(0,9,6,4)));
  voidRoom(plan,1,'nave-air',rect(2,0,4,9),'nave');
  connect(u,'choir-stair',[0,9],[2,9],'stair','choir',true);
  edge(u,'choir-rail',[2,9],[6,9],null,{open:true,rail:true});
  socket(plan,0,'south',[2,0],[6,0],'nave'); socket(plan,0,'north',[2,13],[6,13],'chancel');
  socket(plan,0,'east',[6,3],[6,7],'nave'); socket(plan,0,'west',[0,9],[0,12],'chancel');
  socket(plan,1,'north',[2,13],[6,13],'choir'); socket(plan,1,'east',[6,9],[6,13],'choir');
  socket(plan,1,'west',[0,9],[0,13],'choir');
  plan.stairs.push({id:'choir-stair',lowerLevelId:'ground',upperLevelId:'upper',lowerRoomId:'ground-stair',upperRoomId:'upper-stair',
    width:1.2,tread:.25,maxRiser:.2,headroom:2.2,entryDirection:'N',exitDirection:'N',
    flights:[{id:'ascent',footprint:rect(.4,2.5,1.2,5),direction:'N',stepCount:20}],
    landings:[{id:'lower',kind:'lower',bounds:rect(.4,1.3,1.2,1.2)},{id:'upper',kind:'upper',bounds:rect(.4,7.5,1.2,1.2)}]});
  // Remove the isolated upper deck in front of the flight; retain the ground vestibule.
  plan.verticalVoids.push({id:'choir-stair-front-air',kind:'shaft',lowerLevelId:'ground',upperLevelId:'upper',
    footprint:rect(0,0,2,2.5),roomIds:['ground-stair'],upperRoomIds:['upper-stair']});
}
function service(plan,count) {
  for(let n=0;n<count;n++) {
    const l=plan.levels[n];
    l.rooms.push(room(n,'kitchen',n?'Steward chamber':'Kitchen',n?'chamber':'service',rect(0,0,4,6)),
      room(n,'stair',count===1?'Scullery':'Household stair',count===1?'service':'stair',rect(4,0,6,3)),
      room(n,'pantry',n?'Servants chamber':'Pantry',n?'chamber':'service',rect(4,3,6,3)));
    connect(l,'kitchen-stair',[4,0],[4,3],'kitchen','stair'); connect(l,'kitchen-pantry',[4,3],[4,6],'kitchen','pantry');
    socket(plan,n,'south',[0,0],[4,0],'kitchen'); socket(plan,n,'north',[6,6],[10,6],'pantry');
    socket(plan,n,'west',[0,1],[0,5],'kitchen'); socket(plan,n,'east',[10,3],[10,6],'pantry');
  }
  if(count===2)plan.stairs.push({id:'household-stair',lowerLevelId:'ground',upperLevelId:'upper',lowerRoomId:'ground-stair',upperRoomId:'upper-stair',
    width:1.2,tread:.25,maxRiser:.2,headroom:2.2,entryDirection:'E',exitDirection:'W',flights:[
      {id:'out',direction:'E',stepCount:10,footprint:rect(5.5,.3,2.5,1.2)},
      {id:'return',direction:'W',stepCount:10,footprint:rect(5.5,1.5,2.5,1.2)}],landings:[
      {id:'lower',kind:'lower',bounds:rect(4.3,.3,1.2,1.2)},
      {id:'turn',kind:'intermediate',elevation:2,bounds:rect(8,.3,1.2,2.4)},
      {id:'upper',kind:'upper',bounds:rect(4.3,1.5,1.2,1.2)}]});
}
function gallerySupport(plan,id,from,to,rooms) {
  const role='gallery-rim',levelId='upper';
  plan.beams.push({id,levelId,from,to,section:[.35,.5],jointFamily:'mortise-tenon',role,material:'oak',
    bearing:{kind:'wall-plate',endpoints:[from,to]}});
  const ordered=[from,to].sort((a,b)=>a[0]-b[0]||a[1]-b[1]||a[2]-b[2]);
  const memberId=`beam:upper:${ordered[0].join(',')}|${ordered[1].join(',')}:${role}`;
  for(const name of rooms) {
    const floor=plan.levels[1].rooms.find(r=>r.id===roomId(1,name)).floorStructure;
    (floor.intermediateSupportIds??=[]).push(memberId);
  }
}
function finish(plan,kind) {
  const {width,depth,storeys}=plan.wingProgram;
  if(kind==='hall') {
    gallerySupport(plan,'gallery-west-rim',[2,3.5,2],[2,3.5,6],['west-gallery']);
    gallerySupport(plan,'gallery-south-rim',[0,3.5,2],[10,3.5,2],['south-gallery']);
    gallerySupport(plan,'gallery-north-rim',[0,3.5,6],[10,3.5,6],['north-gallery']);
  }
  if(kind==='chapel')gallerySupport(plan,'choir-rim',[0,3.5,9],[6,3.5,9],['choir']);
  // Window bays must belong to one room and never overlap a connection bay.
  for(const l of plan.levels)for(const r of l.rooms)for(const [side,start,end,fixed] of [
    ['south',r.rect.x,r.rect.x+r.rect.width,r.rect.z],['north',r.rect.x,r.rect.x+r.rect.width,r.rect.z+r.rect.depth],
    ['west',r.rect.z,r.rect.z+r.rect.depth,r.rect.x],['east',r.rect.z,r.rect.z+r.rect.depth,r.rect.x+r.rect.width]]) {
    const horizontal=side==='south'||side==='north';
    if(fixed!==(side==='south'||side==='west'?0:horizontal?depth:width))continue;
    for(let p=start+1;p+2<=end-1;p+=3) {
      const from=horizontal?[p,fixed]:[fixed,p],to=horizontal?[p+2,fixed]:[fixed,p+2],axis=horizontal?0:1;
      if(l.edgeOverrides.some(e=>e.from[1-axis]===fixed&&e.to[1-axis]===fixed&&Math.min(e.from[axis],e.to[axis])<p+2&&Math.max(e.from[axis],e.to[axis])>p))continue;
      edge(l,`${l.id}-${side}-window-${p}`,from,to,null,{window:true});
    }
  }
  const top=levelId(storeys-1),axis=kind==='chapel'?'z':'x';
  plan.roofs.push({id:`${kind}-roof`,levelId:top,kind:kind==='keep'?'hip':'gable',bounds:rect(0,0,width,depth),
    baseY:storeys*4,rise:kind==='keep'?4.6:kind==='chapel'?4.3:kind==='hall'?3.6:2.5,ridgeAxis:axis,
    overhang:.4,material:kind==='service'?'terracotta':'slate',timberMaterial:'oak'});
  const along=axis==='x'?width:depth,count=Math.ceil(along/4);
  for(let i=0;i<=count;i++) {
    const p=along*i/count,y=storeys*4-.12,from=axis==='x'?[p,y,0]:[0,y,p],to=axis==='x'?[p,y,depth]:[width,y,p];
    plan.beams.push({id:`roof-tie-${i}`,levelId:top,from,to,section:[.28,.36],jointFamily:'mortise-tenon',role:'roof-tie',material:'oak',bearing:{kind:'wall-plate',endpoints:[from,to]}});
  }
}
/** Stable socket sourceIds live in plan.wingProgram.sockets. Seal unused exterior
 * entries before site compilation. Only yaw/translation may transform a wing;
 * dimensions are deliberate circulation constraints, not scalable meshes. */
export function castleWingPlan(kind,{id=kind,seed=9411,storeys=CASTLE_WING_DEFAULTS[kind]?.storeys}={}) {
  const defaults=CASTLE_WING_DEFAULTS[kind];
  if(!defaults)throw new Error(`Unknown castle wing kind: ${kind}`);
  if(typeof id!=='string'||!id.length||!Number.isInteger(seed))throw new Error('Wing id must be nonempty and seed an integer');
  if(!Number.isInteger(storeys)||storeys<1||storeys>(kind==='keep'?4:2)||((kind==='hall'||kind==='chapel')&&storeys!==2))
    throw new Error(`Unsupported ${kind} storeys: ${storeys}`);
  const plan={schema:'matter.castle-plan/v1',id,seed,entryRoomId:roomId(0,kind==='keep'?'main':kind==='hall'?'hall':kind==='chapel'?'nave':'kitchen'),
    style:{wallThickness:.6,wallMaterial:'limestone',bond:'ashlar'},
    wingProgram:{kind,...defaults,storeys,sockets:[]},levels:Array.from({length:storeys},(_,n)=>({id:levelId(n),baseY:n*4,height:4,rooms:[],edgeOverrides:[]})),
    stairs:[],verticalVoids:[],beams:[],roofs:[],curves:[],fixtures:[],localLights:[]};
  ({keep,hall,chapel,service}[kind])(plan,storeys); finish(plan,kind); return plan;
}
export function castleWingSocketCatalog(kind,options={}) {
  return castleWingPlan(kind,options).wingProgram.sockets;
}
