// Shared metre-grid room programs. Geometry is emitted by the component libraries.
import { compilePlan } from 'shared-lib/castle_plan';

export const CASTLE_PROGRAMS = {
  "courtyard": {
    "name": "Courtyard residence",
    "width": 36,
    "depth": 40,
    "levels": [
      {
        "y": 0,
        "rooms": [
          ["kitchen","Kitchen",[0,2,8,10],"service"],
          ["hall","Great hall",[8,2,20,10],"hall"],
          ["study","Study",[28,2,8,10],"chamber"],
          ["pantry","Pantry",[0,12,10,8],"service"],
          ["stair","Stair hall",[0,20,10,10],"stair"],
          ["court","Open court",[10,12,16,18],"court"],
          ["chapel","Chapel",[26,12,10,18],"chapel"],
          ["westguard","Guard room",[0,30,14,10],"guard"],
          ["gate","Gate passage",[14,30,8,10],"gallery"],
          ["eastguard","Guard room",[22,30,14,10],"guard"]
        ],
        "portals": [[16,40,20,40],[16,30,20,30],[16,12,20,12],[10,22,10,24],[10,16,10,18],[26,20,26,22],[8,6,8,8],[28,6,28,8],[14,34,14,36],[22,34,22,36]],
        "stairRecords": [
          {
            "id": "west",
            "kind": "straight",
            "lowerY": 0,
            "upperY": 4,
            "flight": [2,22,2,6],
            "direction": [0,1],
            "risers": 20,
            "tread": 0.3,
            "lowerLanding": [2,20,2,2],
            "upperLanding": [2,28,2,2],
            "destinationHole": [2,22,2,6]
          }
        ]
      },
      {
        "y": 4,
        "rooms": [
          ["kitchenloft","Service loft",[0,2,8,10],"service"],
          ["northgallery","Hall balcony",[8,10,20,2],"gallery"],
          ["eaststudy","Library",[28,2,8,10],"chamber"],
          ["westloft","Linen store",[0,12,8,8],"service"],
          ["upperstair","Landing",[0,20,8,10],"stair"],
          ["westgallery","Gallery",[8,12,2,18],"gallery"],
          ["eastgallery","Gallery",[26,12,2,18],"gallery"],
          ["westchamber","West chamber",[0,30,14,10],"chamber"],
          ["southgallery","Solar gallery",[14,30,8,10],"gallery"],
          ["eastchamber","East chamber",[22,30,14,10],"chamber"]
        ],
        "portals": [[2,12,4,12],[2,20,4,20],[8,24,8,26],[2,30,4,30],[14,34,14,36],[22,34,22,36],[26,30,28,30],[28,10,28,12],[8,12,10,12],[26,12,28,12]],
        "stairRecords": [
          {
            "id": "west",
            "kind": "straight",
            "lowerY": 0,
            "upperY": 4,
            "flight": [2,22,2,6],
            "direction": [0,1],
            "risers": 20,
            "tread": 0.3,
            "lowerLanding": [2,20,2,2],
            "upperLanding": [2,28,2,2],
            "destinationHole": [2,22,2,6]
          }
        ]
      }
    ]
  },
  "roundkeep": {
    "name": "Round-tower keep",
    "width": 28,
    "depth": 28,
    "levels": [
      {
        "y": 0,
        "rooms": [
          ["tower","Round stair tower",[-4,0,8,8],"stair","circle"],
          ["lobby","Tower lobby",[4,0,4,8],"gallery"],
          ["kitchen","Kitchen",[8,0,8,8],"service"],
          ["chapel","Oratory",[16,0,8,8],"chapel"],
          ["store","Stores",[0,8,4,14],"service"],
          ["hall","Double-height hall",[4,8,16,14],"hall"],
          ["guard","Guard / service",[20,8,4,14],"guard"],
          ["westentry","Armoury",[0,22,4,6],"guard"],
          ["entry","Entrance hall",[4,22,16,6],"gallery"],
          ["eastentry","Watch room",[20,22,4,6],"guard"]
        ],
        "portals": [[10,28,14,28],[10,22,14,22],[4,3,4,5],[5,8,7,8],[8,3,8,5],[16,3,16,5],[20,14,20,16],[4,14,4,16],[4,24,4,26],[20,24,20,26]],
        "stairRecords": [
          {
            "id": "tower",
            "kind": "return",
            "lowerY": 0,
            "upperY": 4,
            "flightWidths": 1.5,
            "risersPerFlight": 10,
            "tread": 0.25,
            "lowerFlight": [-1.5,2.5,1.5,2.5],
            "upperFlight": [0,2.5,1.5,2.5],
            "lowerDirection": [0,1],
            "upperDirection": [0,-1],
            "middleLanding": [-1.5,5,3,1.5],
            "lowerLanding": [-1.5,1,1.5,1.5],
            "upperLanding": [0,1,1.5,1.5],
            "destinationHole": [-1.5,2.5,3,4]
          }
        ]
      },
      {
        "y": 4,
        "rooms": [
          ["tower","Tower landing",[-4,0,8,8],"stair","circle"],
          ["lobby","Landing",[4,0,4,8],"gallery"],
          ["solar","Solar",[8,0,8,8],"chamber"],
          ["study","Treasury",[16,0,8,8],"chamber"],
          ["westgallery","Hall gallery",[4,8,2,14],"gallery"],
          ["eastgallery","Hall gallery",[18,8,2,14],"gallery"],
          ["southgallery","Gallery",[4,22,16,2],"gallery"],
          ["chambers","Private chambers",[4,24,16,4],"chamber"]
        ],
        "portals": [[4,3,4,5],[8,3,8,5],[16,3,16,5],[4,8,6,8],[18,8,20,8],[10,24,12,24],[4,22,6,22],[18,22,20,22]],
        "stairRecords": [
          {
            "id": "tower",
            "kind": "return",
            "lowerY": 0,
            "upperY": 4,
            "flightWidths": 1.5,
            "risersPerFlight": 10,
            "tread": 0.25,
            "lowerFlight": [-1.5,2.5,1.5,2.5],
            "upperFlight": [0,2.5,1.5,2.5],
            "lowerDirection": [0,1],
            "upperDirection": [0,-1],
            "middleLanding": [-1.5,5,3,1.5],
            "lowerLanding": [-1.5,1,1.5,1.5],
            "upperLanding": [0,1,1.5,1.5],
            "destinationHole": [-1.5,2.5,3,4]
          }
        ]
      }
    ]
  },
  "cloister": {
    "name": "Cloister stronghold",
    "width": 44,
    "depth": 28,
    "levels": [
      {
        "y": 0,
        "rooms": [
          ["weststair","West stair",[0,0,8,8],"stair"],
          ["refectory","Refectory",[8,0,22,8],"hall"],
          ["kitchen","Kitchen",[30,0,8,8],"service"],
          ["pantry","Pantry",[38,0,6,8],"service"],
          ["chapter","Chapter house",[0,8,8,12],"chamber"],
          ["northwalk","Cloister",[8,8,26,2],"gallery"],
          ["westwalk","Cloister",[8,10,2,8],"gallery"],
          ["eastwalk","Cloister",[32,10,2,8],"gallery"],
          ["court","Open court",[10,10,22,8],"court"],
          ["southwalk","Cloister",[8,18,26,2],"gallery"],
          ["chapel","Chapel",[34,8,10,12],"chapel"],
          ["westguard","Gate guard",[0,20,12,8],"guard"],
          ["gate","Gate passage",[12,20,8,8],"gallery"],
          ["stores","Stores",[20,20,16,8],"service"],
          ["eaststair","East stair",[36,20,8,8],"stair"]
        ],
        "portals": [
          [14,28,18,28],
          [14,20,18,20],
          [8,14,8,16],
          [8,3,8,5],
          [16,8,20,8],
          [30,3,30,5],
          [38,3,38,5],
          [34,13,34,15],
          [38,20,40,20],
          [36,23,36,25],
          [20,23,20,25],
          [12,23,12,25],
          [8,10,10,10],
          [32,10,34,10],
          [8,18,10,18],
          [32,18,34,18]
        ],
        "stairRecords": [
          {
            "id": "west",
            "kind": "return",
            "lowerY": 0,
            "upperY": 4,
            "flightWidths": 2,
            "risersPerFlight": 10,
            "tread": 0.3,
            "lowerFlight": [2,2,2,3],
            "upperFlight": [4,2,2,3],
            "lowerDirection": [0,1],
            "upperDirection": [0,-1],
            "middleLanding": [2,5,4,2],
            "lowerLanding": [2,0,2,2],
            "upperLanding": [4,0,2,2],
            "destinationHole": [2,2,4,5]
          },
          {
            "id": "east",
            "kind": "return",
            "lowerY": 0,
            "upperY": 4,
            "flightWidths": 2,
            "risersPerFlight": 10,
            "tread": 0.3,
            "lowerFlight": [38,22,2,3],
            "upperFlight": [40,22,2,3],
            "lowerDirection": [0,1],
            "upperDirection": [0,-1],
            "middleLanding": [38,25,4,2],
            "lowerLanding": [38,20,2,2],
            "upperLanding": [40,20,2,2],
            "destinationHole": [38,22,4,5]
          }
        ]
      },
      {
        "y": 4,
        "rooms": [
          ["westlanding","Landing",[0,0,8,8],"stair"],
          ["dormitory","Dormitory",[8,0,22,8],"chamber"],
          ["library","Library",[30,0,14,8],"chamber"],
          ["chamber","Guest chamber",[0,8,8,12],"chamber"],
          ["northwalk","Upper gallery",[8,8,26,2],"gallery"],
          ["westwalk","Upper gallery",[8,10,2,8],"gallery"],
          ["eastwalk","Upper gallery",[32,10,2,8],"gallery"],
          ["southwalk","Upper gallery",[8,18,26,2],"gallery"],
          ["chapelgallery","Chapel gallery",[34,8,2,12],"gallery"],
          ["westsolar","Solar",[0,20,12,8],"chamber"],
          ["gateloft","Gate loft",[12,20,8,8],"gallery"],
          ["workroom","Scriptorium",[20,20,16,8],"chamber"],
          ["eastlanding","Landing",[36,20,8,8],"stair"],
          ["choir","Choir loft",[36,18,8,2],"gallery"]
        ],
        "portals": [
          [8,3,8,5],
          [30,3,30,5],
          [16,8,20,8],
          [34,13,34,15],
          [38,20,40,20],
          [36,23,36,25],
          [20,23,20,25],
          [12,23,12,25],
          [14,20,18,20],
          [8,14,8,16],
          [8,10,10,10],
          [32,10,34,10],
          [8,18,10,18],
          [32,18,34,18],
          [36,18,36,20]
        ],
        "stairRecords": [
          {
            "id": "west",
            "kind": "return",
            "lowerY": 0,
            "upperY": 4,
            "flightWidths": 2,
            "risersPerFlight": 10,
            "tread": 0.3,
            "lowerFlight": [2,2,2,3],
            "upperFlight": [4,2,2,3],
            "lowerDirection": [0,1],
            "upperDirection": [0,-1],
            "middleLanding": [2,5,4,2],
            "lowerLanding": [2,0,2,2],
            "upperLanding": [4,0,2,2],
            "destinationHole": [2,2,4,5]
          },
          {
            "id": "east",
            "kind": "return",
            "lowerY": 0,
            "upperY": 4,
            "flightWidths": 2,
            "risersPerFlight": 10,
            "tread": 0.3,
            "lowerFlight": [38,22,2,3],
            "upperFlight": [40,22,2,3],
            "lowerDirection": [0,1],
            "upperDirection": [0,-1],
            "middleLanding": [38,25,4,2],
            "lowerLanding": [38,20,2,2],
            "upperLanding": [40,20,2,2],
            "destinationHole": [38,22,4,5]
          }
        ]
      }
    ]
  }
};

const bounds=([x,z,width,depth])=>({x,z,width,depth});
const roomId=(i,id)=>`${i===0?'g':'u'}-${id}`;
function contains(r,x,z){if(r[4]==='circle')return false;const [a,b,w,d]=r[2];return x>a&&x<a+w&&z>b&&z<b+d;}
export function castleBasePlan(name = 'courtyard', seed = 9411) {
 const program = CASTLE_PROGRAMS[name];
 if (!program) throw new Error('Unknown castle variation: ' + name);
 const plan={id:name,seed,entryRoomId:name==='roundkeep'?'g-entry':'g-gate',style:{wallThickness:.6,wallMaterial:'limestone',bond:'ashlar'},levels:[],stairs:[],beams:[],fixtures:[],roofs:[],localLights:[],curves:[],verticalVoids:[]};
 for(const [index,level] of program.levels.entries()){
  const levelId=index===0?'ground':'upper';
  const rooms=level.rooms.map(([id,label,r,use,shape])=>({id:roomId(index,id),label,use,floorType:index===0?'flags':'oak',...(shape==='circle'?{boundary:{kind:'circle',center:[r[0]+r[2]/2,r[1]+r[3]/2],radius:r[2]/2}}:{rect:bounds(r)})}));
  const portalSegments=[...level.portals];
  if(name==='cloister'&&index===0)portalSegments.push([14,10,16,10],[22,10,24,10],[32,12,32,14]);
  const edgeOverrides=portalSegments.map(([x1,z1,x2,z2])=>{
   const dx=x2-x1,dz=z2-z1,len=Math.hypot(dx,dz),x=(x1+x2)/2,z=(z1+z2)/2;
   const side=[-1,1].map(sign=>level.rooms.find(r=>contains(r,x+sign*(-dz)/len*.01,z+sign*dx/len*.01)));
   const connects=side.map(r=>r?roomId(index,r[0]):'outside');
   const open=side.every(r=>r&&r[3]==='gallery');
   return {id:`${levelId}-portal-${x1}-${z1}-${x2}-${z2}`,from:[x1,z1],to:[x2,z2],kind:open?'open':'arch',connects,opening:{offset:open?0:.35,width:len-(open?0:.7),height:open?4:2.8,bottom:0}};
  });
  plan.levels.push({id:levelId,baseY:level.y,height:4,rooms,edgeOverrides});
  if(name==='roundkeep')plan.curves.push({id:`${levelId}-tower`,levelId,roomId:roomId(index,'tower'),kind:'ring',center:[0,4],radius:4,thickness:.6,height:4,apertures:[{id:`${levelId}-throat`,kind:'arch',startAngle:-16,endAngle:16,connects:[roomId(index,'tower'),roomId(index,'lobby')],bottom:0,height:2.8,throat:{targetRoomId:roomId(index,'lobby'),direction:'E',width:1.3,depth:.6}}]});
 }
 if(name==='courtyard')plan.stairs.push({id:'west-stair',lowerLevelId:'ground',upperLevelId:'upper',lowerRoomId:'g-stair',upperRoomId:'u-upperstair',width:2,tread:.3,maxRiser:.2,headroom:2.2,entryDirection:'N',exitDirection:'N',flights:[{id:'west-flight',footprint:{x:2,z:22,width:2,depth:6},direction:'N',stepCount:20}],landings:[{id:'west-lower',kind:'lower',bounds:{x:2,z:20,width:2,depth:2}},{id:'west-upper',kind:'upper',bounds:{x:2,z:28,width:2,depth:2}}]});
 else for(const s of program.levels[0].stairRecords){
  const lowerRoomId=name==='roundkeep'?'g-tower':`g-${s.id}stair`,upperRoomId=name==='roundkeep'?'u-tower':`u-${s.id}landing`;
  plan.stairs.push({id:s.id,lowerLevelId:'ground',upperLevelId:'upper',lowerRoomId,upperRoomId,width:s.flightWidths,tread:s.tread,maxRiser:.2,headroom:2.2,entryDirection:'N',exitDirection:'S',flights:[{id:'lower-flight',footprint:bounds(s.lowerFlight),direction:'N',stepCount:s.risersPerFlight},{id:'upper-flight',footprint:bounds(s.upperFlight),direction:'S',stepCount:s.risersPerFlight}],landings:[{id:'lower-landing',kind:'lower',bounds:bounds(s.lowerLanding)},{id:'middle-landing',kind:'intermediate',elevation:2,bounds:bounds(s.middleLanding)},{id:'upper-landing',kind:'upper',bounds:bounds(s.upperLanding)}]});
 }
 if(name==='courtyard')plan.verticalVoids.push({id:'great-hall',kind:'double-height',lowerLevelId:'ground',upperLevelId:'upper',footprint:{x:8,z:2,width:20,depth:8},roomIds:['g-hall']},{id:'chapel',kind:'double-height',lowerLevelId:'ground',upperLevelId:'upper',footprint:{x:28,z:12,width:8,depth:18},roomIds:['g-chapel']});
 if(name==='roundkeep')plan.verticalVoids.push({id:'great-hall',kind:'double-height',lowerLevelId:'ground',upperLevelId:'upper',footprint:{x:6,z:8,width:12,depth:14},roomIds:['g-hall']});
 if(name==='cloister')plan.verticalVoids.push({id:'chapel',kind:'double-height',lowerLevelId:'ground',upperLevelId:'upper',footprint:{x:36,z:8,width:8,depth:10},roomIds:['g-chapel']});

 for (const voidRecord of plan.verticalVoids) {
  const level = plan.levels.find(item => item.id === voidRecord.upperLevelId);
  level.rooms.push({id:'u-air-' + voidRecord.id, use:'void', required:false, openToBelow:true, lowerRoomId:voidRecord.roomIds[0], rect:{...voidRecord.footprint}});
 }
 for (const level of plan.levels) for (const room of level.rooms) {
  if (level.baseY > 0 && room.rect && !room.openToBelow) room.floorStructure = {joistDirection:room.rect.width <= room.rect.depth ? 'x' : 'z', joistSpacing:.6};
 }
 return plan;
}

export function castlePlan(name = 'courtyard', seed = 9411) {
 const plan = castleBasePlan(name, seed);
 addGalleryEdges(plan);
 addFacadeBays(plan);
 addRoofsAndTimber(plan, name);
 addInteriorProgram(plan);
 return plan;
}

export function castleManifest(name = 'courtyard', seed = 9411) {
 return compilePlan(castlePlan(name, seed));
}

// Derive repeated facade bays from canonical metre edges, keeping authored portals intact.
function addFacadeBays(plan) {
 const base = compilePlan(plan);
 const levels = new Map(plan.levels.map(level => [level.id, level]));
 const rooms = new Map(base.rooms.map(room => [room.id, room]));
 const edgeMap = new Map(base.walls.map(wall => [wall.levelId + ':' + wall.from.join(',') + ':' + wall.axis, wall]));
 for (const wall of base.walls) {
  const axis = wall.axis === 'x' ? 0 : 1;
  if (wall.kind !== 'wall' || ((wall.from[axis] % 4) + 4) % 4 !== 1) continue;
  const next = edgeMap.get(wall.levelId + ':' + wall.to.join(',') + ':' + wall.axis);
  if (!next || next.kind !== 'wall' || next.roomIds.join('|') !== wall.roomIds.join('|')) continue;
  if (wall.startSocket.junctionKind !== 'straight' || next.endSocket.junctionKind !== 'straight') continue;
  const adjacent = wall.roomIds.map(id => rooms.get(id));
  const courtArcade = adjacent.some(room => room.use === 'court') && adjacent.some(room => room.use === 'gallery');
  if (wall.boundary !== 'exterior' && !courtArcade) continue;
  const level = levels.get(wall.levelId);
  const kind = courtArcade ? 'arch' : 'window';
  level.edgeOverrides.push({id:'bay-' + wall.id, from:[...wall.from], to:[...next.to], kind,
   ...(courtArcade ? {connects:[...wall.roomIds]} : {}),
   opening:{offset:.25, width:1.5, bottom:courtArcade?0:1.05, height:courtArcade?3.1:2.1}});
 }
 for (const curve of plan.curves) for (const angle of [90,180,270])
  curve.apertures.push({id:curve.id+'-window-'+angle, kind:'window', startAngle:angle-9,
   endAngle:angle+9, bottom:1.1, height:1.9});
}

// An upper gallery edge opens only where it overlooks air: a double-height
// void beside it, or an open court below its exterior side. The court test
// samples the cell beyond the wall, not the wall line: a gallery wall on a
// court's edge overlooks it, but one on a hall's outer edge is the keep's
// outer wall above the lean-tos.
function addGalleryEdges(plan) {
 const base = compilePlan(plan);
 const courts = plan.levels[0].rooms.filter(room => room.rect && room.use === 'court');
 const rooms = new Map(base.rooms.map(room => [room.id, room]));
 const cells = new Map(base.rooms.map(room => [room.id, new Set((room.boundary.cells || []).map(cell => cell.join(',')))]));
 const groups = new Map();
 for (const wall of base.walls) {
  const adjacent = wall.roomIds.map(id => rooms.get(id));
  const gallery = adjacent.find(room => room.use === 'gallery');
  if (wall.levelId !== 'upper' || wall.kind !== 'wall' || !gallery) continue;
  let overVoid = adjacent.some(room => room.openToBelow);
  if (!overVoid && wall.boundary === 'exterior') {
   const [x,z] = wall.from.map((value,i) => Math.min(value,wall.to[i]));
   const sides = wall.axis === 'x' ? [[x,z-1],[x,z]] : [[x-1,z],[x,z]];
   const [cx,cz] = sides.find(cell => !cells.get(gallery.id).has(cell.join(',')));
   overVoid = courts.some(({rect:r}) => cx >= r.x && cx < r.x+r.width && cz >= r.z && cz < r.z+r.depth);
  }
  if (!overVoid) continue;
  const axis=wall.axis==='x'?0:1;
  const key=gallery.id+':'+wall.axis+':'+wall.from[1-axis];
  if(!groups.has(key))groups.set(key,[]);
  groups.get(key).push(wall);
 }
 const upper=plan.levels.find(level=>level.id==='upper');
 for(const walls of groups.values()){
  const axis=walls[0].axis==='x'?0:1;
  walls.sort((a,b)=>a.from[axis]-b.from[axis]);
  let run=[];
  const flush=()=>{
   if(!run.length)return;
   const from=run[0].from,to=run[run.length-1].to;
   upper.edgeOverrides.push({id:'balcony-'+run[0].id,from:[...from],to:[...to],kind:'open',railProfile:'castle.oak-guardrail',opening:{offset:0,width:to[axis]-from[axis],height:4,bottom:0}});
   run=[];
  };
  for(const wall of walls){if(run.length && run[run.length-1].to[axis]!==wall.from[axis])flush();run.push(wall);}
  flush();
 }
}

// Roof footprints follow whole building wings, rather than repeating a roof per room.
// The different ridge heights make the chapel, gate and stair tower legible outside.
function addRoofsAndTimber(plan, name) {
 const roof = (id, rect, rise, ridgeAxis='x', kind='gable', baseY=8, material='slate') =>
  plan.roofs.push({id,levelId:baseY>=8?'upper':'ground',kind,
   bounds:{x:rect[0],z:rect[1],width:rect[2],depth:rect[3]},baseY,rise,
   ridgeAxis,overhang:.45,material,timberMaterial:'oak'});
 if (name==='courtyard') {
  roof('great-hall-wing',[0,2,36,10],4.5);
  roof('west-wing',[0,12,10,18],3,'z');
  roof('chapel-wing',[26,12,10,18],5,'z');
  roof('west-solar',[0,30,14,10],3.8);
  roof('gatehouse',[14,30,8,10],5.8,'z','hip');
  roof('east-solar',[22,30,14,10],3.8);
 } else if(name==='roundkeep') {
  roof('north-wing',[4,0,20,8],3.6);
  roof('hall',[4,8,16,16],6,'z','hip');
  roof('private-chambers',[4,24,16,4],2.4);
  roof('west-service',[0,8,4,20],1.8,'z','gable',4);
  roof('east-service',[20,8,4,20],1.8,'z','gable',4);
  plan.roofs.push({id:'stair-tower',levelId:'upper',kind:'conical',center:[0,4],
   radius:4,baseY:8,rise:7,overhang:.5,material:'terracotta',timberMaterial:'oak'});
 } else {
  roof('north-range',[0,0,44,8],3.6,'x','gable',8,'terracotta');
  roof('west-range',[0,8,8,12],3.6,'z','gable',8,'terracotta');
  roof('south-range',[0,20,44,8],3.6,'x','gable',8,'terracotta');
  roof('chapel',[34,8,10,12],6,'z');
  roof('north-cloister',[8,8,26,2],1.1,'x');
  roof('south-cloister',[8,18,26,2],1.1,'x');
  roof('west-cloister',[8,10,2,8],1.1,'z');
  roof('east-cloister',[32,10,2,8],1.1,'z');
 }
 // Roof ties span between real perimeter bearings. Keep their underside above
 // the upper-storey headroom envelope, including the stair arrivals.
 const seen = new Set();
 const beam=(id,from,to,section,role,levelId='upper')=>{
  const key=[from.join(','),to.join(',')].sort().join('|');
  if(seen.has(key))return;seen.add(key);
  plan.beams.push({id,levelId,from,to,section,role,jointFamily:'mortise-tenon',
   material:'oak',bearing:{kind:'wall-plate',endpoints:[from,to]}});
 };
 for(const r of plan.roofs) {
  if(!r.bounds)continue;
  const b=r.bounds, alongX=r.ridgeAxis==='x';
  const length=alongX?b.width:b.depth,span=alongX?b.depth:b.width;
  const count=Math.max(1,Math.ceil(length/4));
  for(let i=0;i<=count;i++){
   const along=length*i/count;
   const a=alongX?[b.x+along,r.baseY+(r.baseY>=8?.36:-.5),b.z]:[b.x,r.baseY+(r.baseY>=8?.36:-.5),b.z+along];
   const c=alongX?[b.x+along,r.baseY+(r.baseY>=8?.36:-.5),b.z+span]:[b.x+span,r.baseY+(r.baseY>=8?.36:-.5),b.z+along];
   beam(r.id+'-tie-'+i,a,c,[.28,span>10?.48:.36],'roof-tie',r.levelId);
  }
 }
}

// Furnishings reserve actual floor space. The compiler's swept walking routes
// remain clear, so a furnished room cannot silently invalidate the floor plan.
function addInteriorProgram(plan) {
 const manifest=compilePlan(plan), used=[];
 const overlap=(a,b)=>a.minX<b.maxX && a.maxX>b.minX && a.minY<b.maxY && a.maxY>b.minY && a.minZ<b.maxZ && a.maxZ>b.minZ;
 const routes=manifest.walkRoute.flatMap(route=>route.roomSegments.flatMap(room=>room.segments.map(leg=>leg.bounds)));
 const reserved=[...routes,...manifest.portals.map(p=>p.bounds),
  ...manifest.occupiedVolumes.filter(v=>v.kind==='stair-clearance').map(v=>v.bounds)];
 const floorByRoom=new Map(manifest.floors.map(f=>[f.roomId,f]));
 const place=(room,level,kind,x,z,width,depth,height,yaw=0)=>{
  const y=level.baseY, clearance={minX:x-width/2-.12,maxX:x+width/2+.12,minZ:z-depth/2-.12,maxZ:z+depth/2+.12,minY:y,maxY:y+height};
  if([...reserved,...used].some(b=>overlap(clearance,b)))return false;
  const floor=floorByRoom.get(room.id);
  if(!floor)return false;
  if((floor.holes||[]).some(h=>h.kind!=='ceiling' && (h.regions||[h]).some(r=>{
   const q=r.bounds||r;return q.minX!==undefined?overlap(clearance,{...q,minY:y,maxY:y+height}):
    q.x!==undefined && overlap(clearance,{minX:q.x,maxX:q.x+q.width,minZ:q.z,maxZ:q.z+q.depth,minY:y,maxY:y+height});
  })))return false;
  const r=room.rect;
  if(!r || clearance.minX<r.x+.4 || clearance.maxX>r.x+r.width-.4 || clearance.minZ<r.z+.4 || clearance.maxZ>r.z+r.depth-.4)return false;
  used.push(clearance);
  plan.fixtures.push({id:room.id+'-'+kind+'-'+plan.fixtures.length,levelId:level.id,roomId:room.id,
   kind,position:[x,y,z],yaw,width,depth,height,seed:plan.seed+plan.fixtures.length,clearance});
  return true;
 };
 for(const level of plan.levels)for(const room of level.rooms){
  if(!room.rect || room.openToBelow || ['stair','court','gallery'].includes(room.use))continue;
  const r=room.rect, candidates=[];
  for(let z=r.z+1.5;z<r.z+r.depth-1;z+=2.5)for(let x=r.x+2;x<r.x+r.width-1.5;x+=3.5)candidates.push([x,z]);
  const desired=room.use==='hall'?[['table',3.4,1.1,.85],['bench',3.2,.45,.5],['table',3.4,1.1,.85],['bench',3.2,.45,.5],['cabinet',1.6,.6,1.7]]:
   room.use==='chapel'?[['altar',2.4,1.1,1.1],['bench',2.4,.45,.5],['bench',2.4,.45,.5],['bench',2.4,.45,.5]]:
   room.use==='chamber'?[['bed',2.2,1.5,1.2],['cabinet',1.5,.6,1.7],['table',1.6,.9,.85],['chair',.6,.6,1.1],['chest',1.1,.6,.65]]:
   room.use==='guard'?[['table',2.2,1,.85],['bench',2,.45,.5],['chest',1.1,.6,.65]]:
   [['barrel',.8,.8,1.05],['barrel',.8,.8,1.05],['cabinet',1.5,.6,1.7],['table',2.2,1,.85],['chest',1.1,.6,.65]];
  for(const [kind,w,d,h] of desired)for(const [x,z] of candidates)if(place(room,level,kind,x,z,w,d,h))break;
 }
 // Wall lamps attach only to solid wall bays, facing an occupied interior.
 const rooms=new Map(plan.levels.flatMap(l=>l.rooms.map(r=>[r.id,{...r,baseY:l.baseY,levelId:l.id}])));
 const counts=new Map();
 for(const wall of manifest.walls){
  if(wall.kind!=='wall')continue;
  const room=wall.roomIds.map(id=>rooms.get(id)).find(r=>r && r.rect && !r.openToBelow && r.use!=='court');
  if(!room || (counts.get(room.id)||0)>=3)continue;
  const along=wall.axis==='x'?wall.from[0]:wall.from[1];
  if(((along%5)+5)%5!==2)continue;
  const cx=(wall.from[0]+wall.to[0])/2,cz=(wall.from[1]+wall.to[1])/2;
  const nx=wall.axis==='z'?Math.sign(room.rect.x+room.rect.width/2-cx):0;
  const nz=wall.axis==='x'?Math.sign(room.rect.z+room.rect.depth/2-cz):0;
  const position=[cx+nx*.4,room.baseY+2.55,cz+nz*.4],id='lamp-'+wall.id;
  plan.fixtures.push({id,levelId:room.levelId,roomId:room.id,kind:'sconce',position,
   yaw:Math.atan2(nx,nz)*180/Math.PI,seed:plan.seed+plan.fixtures.length,lightId:id+'-point'});
  plan.localLights.push({id:id+'-point',levelId:room.levelId,kind:'point',
   position:[position[0]+nx*.18,position[1]+.2,position[2]+nz*.18],color:[1,.62,.27],
   intensity:18,range:6.5,sourceRadius:.08,castsShadow:true,fixtureId:id});
  counts.set(room.id,(counts.get(room.id)||0)+1);
 }
 // Tall spaces get a suspended centrepiece and a separate downward accent.
 for(const level of plan.levels)for(const room of level.rooms){
  if(!room.rect || !['hall','chapel'].includes(room.use))continue;
  const r=room.rect,x=r.x+r.width/2,z=r.z+r.depth/2;
  const tall=plan.verticalVoids.some(v=>v.roomIds.includes(room.id));
  const y=level.baseY+(tall?5.3:2.75),id=room.id+'-chandelier';
  plan.fixtures.push({id,levelId:level.id,roomId:room.id,kind:'chandelier',position:[x,y,z],yaw:0,seed:plan.seed,lightId:id+'-point'});
  plan.localLights.push({id:id+'-point',levelId:level.id,kind:'point',position:[x,y-.15,z],color:[1,.72,.38],intensity:36,range:tall?12:8,sourceRadius:.2,castsShadow:true,fixtureId:id});
  plan.localLights.push({id:id+'-spot',levelId:level.id,kind:'spot',position:[x,y+.4,z],direction:[0,-1,0],color:[1,.84,.61],intensity:24,range:12,inner:30,outer:55,sourceRadius:.12,castsShadow:true,fixtureId:id});
 }
}
