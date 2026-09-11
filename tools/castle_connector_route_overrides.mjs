#!/usr/bin/env node
// Generate authored connector detours; no editor or geometry changes. The batch
// caller discovers the missing connectors and supplies one existing target stair.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import crypto from 'node:crypto';
await import('../projects/world_demo/tests/castle_shared_lib_hooks.mjs');
const {routeManifestRoomSegment}=await import('../projects/world_demo/shared-lib/castle_plan.js');
const {inverseTransformPoint,transformPoint}=await import('../projects/world_demo/shared-lib/castle_frames.js');
const near=(a,b)=>a.every((v,i)=>Math.abs(v-b[i])<1e-5);
function append(points,sequence){for(const point of sequence)if(!points.length||!near(points.at(-1),point))points.push([...point]);}
function need(condition,message){if(!condition)throw new Error(message);}

export function connectorDetour(site,connectorId,stairId){
 need(site.schema==='matter.castle-site-manifest/v1','detour requires a compiled site manifest');
 const connector=site.connectors.find(c=>c.id===connectorId);
 need(connector?.mouths.length===2,'connector must have exactly two authored mouths');
 const [a,b]=connector.mouths;
 const wing=site.wings.find(w=>w.id===a.wing);
 need(wing,'connector approach wing missing');
 const approach=site.walkRoutes.find(r=>r.roomId===a.wing+':'+a.roomId);
 need(approach,'authored entry route to connector approach room missing');
 const prefix=[];append(prefix,[site.spawn]);append(prefix,approach.waypoints);
 const roomSegment=routeManifestRoomSegment(wing.manifest,a.roomId,
  inverseTransformPoint(wing.frame,prefix.at(-1)),inverseTransformPoint(wing.frame,a.inside));
 append(prefix,roomSegment.waypoints.map(p=>transformPoint(wing.frame,p)));
 need(near(prefix.at(-1),a.inside),'approach must end at authored inside mouth');
 need(near(connector.routeWaypoints[0],a.inside)&&near(connector.routeWaypoints.at(-1),b.inside),
  'connector route must preserve the ordered inside-mouth endpoints');
 const outbound=prefix.map(p=>[...p]);append(outbound,connector.routeWaypoints);
 const waypoints=outbound.map(p=>[...p]);append(waypoints,[...outbound].reverse());
 need(near(waypoints.at(-1),site.spawn),'detour must return continuously to authored spawn');
 let selected,owner;
 for(const candidate of site.wings)for(const stair of candidate.manifest.stairs)
  if(candidate.id+':'+stair.id===stairId){selected=stair;owner=candidate;}
 need(selected,'qualified target staircase missing');
 const stairRoute=site.walkRoutes.find(r=>r.roomId===owner.id+':'+selected.upperRoomId);
 need(stairRoute,'authored route to selected upper stair room missing');
 append(waypoints,stairRoute.waypoints);
 return {schema:'matter.castle-connector-route/v1',connector_id:connectorId,stair_id:stairId,
  waypoints,connector_waypoints:connector.routeWaypoints.map(p=>[...p]),
  approach_room_id:approach.roomId,approach_route_edge_ids:approach.edgeIds,
  approach_room_segment:roomSegment,outbound_waypoints:outbound,
  note:'Authored entry route, compiler-checked room join, full connector, reverse to spawn, then full existing stair route. Planned proof only.'};
}

if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url)){
 const request=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));
 for(const item of request){
  const bytes=fs.readFileSync(item.manifest),site=JSON.parse(bytes);
  const route=connectorDetour(site,item.connector_id,item.stair_id);
  route.source_manifest_sha256=crypto.createHash('sha256').update(bytes).digest('hex');
  fs.mkdirSync(path.dirname(item.output),{recursive:true});
  fs.writeFileSync(item.output,JSON.stringify(route,null,2)+'\n');
 }
}
