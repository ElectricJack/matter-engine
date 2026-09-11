// Architectural programs for connected, differently oriented building wings.
// All dimensions inside a wing remain on its own metre grid. Site placement
// solves named door sockets; these are not overlapping rotated building boxes.
import { castleWingPlan } from 'shared-lib/castle_wing_programs';

export const CASTLE_SITE_NAMES=['clustered-court','angled-bailey','bent-palace'];
export const CASTLE_SITE_SEEDS=[9411,17029,28303];
const socket=(wing,side,level='ground')=>({wing,level,portal:(level==='ground'?'':level+'-')+side+'-entry'});
const localSocket=side=>({level:'ground',portal:side+'-entry'});
const placement=(side,targetWing,targetSide,yawDeg,outset=6,lateral=0)=>({
 socket:localSocket(side),relativeTo:socket(targetWing,targetSide),yawDeg,outset,lateral,
});
function link(id,aWing,aSide,bWing,bSide){
 return {id,a:socket(aWing,aSide),b:socket(bWing,bSide),floor:'stone',height:3.6,
  roof:{kind:'low-hip',rise:.8}};
}
function sealUnusedDoors(site){
 const used=new Set();
 const mark=s=>used.add(s.wing+':'+s.level+':'+s.portal);
 mark(site.entry);
 for(const c of site.connections){mark(c.a);mark(c.b);}
 for(const wing of site.wings)for(const level of wing.plan.levels){
  level.edgeOverrides=level.edgeOverrides.map(edge=>{
   if(!/(?:^|-)(?:north|south|east|west)-entry$/.test(edge.id)||
      used.has(wing.id+':'+level.id+':'+edge.id))return edge;
   // An unused upper socket is a glazed opening, never a door onto empty air.
   const length=Math.hypot(edge.to[0]-edge.from[0],edge.to[1]-edge.from[1]);
   const {connects,...rest}=edge;
   return {...rest,kind:'window',opening:{offset:(length-1.5)/2,width:1.5,bottom:1.1,height:1.9}};
  });
 }
 return site;
}
export function castleSitePlan(name='clustered-court',seed=9411){
 const index=CASTLE_SITE_NAMES.indexOf(name);
 if(index<0)throw new Error('Unknown castle site '+name);
 let sequence=0;
 const wing=(id,kind,position,storeys)=>({id,
  plan:castleWingPlan(kind,{id:name+'-'+id,seed:seed+(++sequence)*101,
   ...(storeys===undefined?{}:{storeys})}),...position});
 const site={schema:'matter.castle-site/v1',id:name,seed,grid:1,angleStep:15,
  entry:socket('keep','south'),wings:[],connections:[]};
 if(name==='clustered-court'){
  site.wings=[
   wing('keep','keep',{frame:{origin:[0,0,0],yawDeg:0}},3),
   wing('hall','hall',{placement:placement('west','keep','east',30)},2),
   wing('chapel','chapel',{placement:placement('south','hall','north',-15)},2),
   wing('service','service',{placement:placement('south','keep','north',15)},1),
  ];
  site.connections=[link('keep-hall','keep','east','hall','west'),
   link('hall-chapel','hall','north','chapel','south'),
   link('keep-service','keep','north','service','south'),
   link('service-chapel','service','east','chapel','west')];
 }else if(name==='angled-bailey'){
  // The tall keep sits beyond the western hall. Lower buildings and covered
  // curtain passages frame a broad bailey, with several independent wall axes.
  site.entry=socket('gate','south');
  site.wings=[
   wing('gate','service',{frame:{origin:[0,0,0],yawDeg:0}},2),
   wing('hall','hall',{placement:placement('east','gate','west',-15)},2),
   wing('keep','keep',{placement:placement('south','hall','north',45)},3),
   wing('service','service',{placement:placement('west','gate','east',15)},1),
   wing('chapel','chapel',{placement:placement('south','service','north',30,10)},2),
  ];
  site.connections=[link('gate-hall','gate','west','hall','east'),
   link('hall-keep','hall','north','keep','south'),
   link('gate-service','gate','east','service','west'),
   link('service-chapel','service','north','chapel','south'),
   link('north-curtain','keep','north','chapel','west')];
 }else{
  // Successive changes of axis form an elongated palace. Lower western
  // service ranges enclose two courts along the taller hall/chapel spine.
  site.wings=[
   wing('keep','keep',{frame:{origin:[0,0,0],yawDeg:0}},2),
   wing('hall','hall',{placement:placement('south','keep','north',15,5)},2),
   wing('solar','hall',{placement:placement('south','hall','north',30,5)},2),
   wing('chapel','chapel',{placement:placement('south','solar','north',45,5)},2),
   wing('kitchen','service',{placement:placement('east','hall','west',15)},1),
   wing('service','service',{placement:placement('east','solar','west',30)},1),
  ];
  site.connections=[link('keep-hall','keep','north','hall','south'),
   link('hall-solar','hall','north','solar','south'),
   link('solar-chapel','solar','north','chapel','south'),
   link('hall-kitchen','hall','west','kitchen','east'),
   link('solar-service','solar','west','service','east'),
   link('lower-court','keep','west','kitchen','south'),
   link('upper-court','kitchen','north','service','south')];
 }
 return sealUnusedDoors(site);
}
