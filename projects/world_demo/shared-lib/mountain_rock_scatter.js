import { HABITAT } from 'shared-lib/alpine_ecology';
import { mountainRockReferenceForSize } from 'shared-lib/mountain_rock_sizes';

const clamp=x=>Math.max(0,Math.min(1,x));
const fade=(a,b,x)=>{const t=clamp((x-a)/(b-a));return t*t*(3-2*t);};
const fract=x=>x-Math.floor(x);
export const MOUNTAIN_ROCK_HALO=64;

// Candidates, cluster anchors and decisions depend only on world coordinates.
// A caller can plan a halo for vegetation exclusion and emit just its half-open
// cell. This keeps ownership and poses stable as streaming tiles change size.
export function planMountainRocks({worldSeed,ox,oz,sectorSize=64,detail=2,
    candidatesInRect,habitatAt,biomeAt,heightAt,available=()=>true}) {
  if(!habitatAt) return [];
  const sample=[],out=[],seed=worldSeed>>>0;
  const terrain=(x,z)=>{
    if(biomeAt(x,z)==='ocean')return null;
    habitatAt(x,z,sample);
    const height=sample[HABITAT.altitude],slope=sample[HABITAT.slope];
    if(!Number.isFinite(height)||!Number.isFinite(slope)||height<0||height>520||slope>.48)return null;
    return {height,slope};
  };
  const emit=(c,size,kind,t)=>{
    const shape=kind==='scree'?1:Math.min(2,Math.floor(c.v*3));
    const rockSeed=Math.min(3,Math.floor(fract(c.v*19.73)*4));
    const referenceSizeM=mountainRockReferenceForSize(size);
    // A conservative prototype horizontal bound, shared by exclusion/placement.
    const radius=size*1.5;
    if(!available(c.x,c.z,radius))return;
    let pitch=0,roll=0,groundY=t.height;
    if(heightAt) {
      groundY=heightAt(c.x,c.z);
      const d=Math.max(.5,Math.min(4,size*.35));
      const gx=(heightAt(c.x+d,c.z)-heightAt(c.x-d,c.z))/(2*d);
      const gz=(heightAt(c.x,c.z+d)-heightAt(c.x,c.z-d))/(2*d);
      if(![groundY,gx,gz].every(Number.isFinite))return;
      const yaw=c.rot;
      roll=Math.atan(Math.cos(yaw)*gx-Math.sin(yaw)*gz);
      // Placement applies Ry * Rz * Rx. The already-applied roll changes the
      // vertical component against which the remaining local Z slope is fit.
      pitch=-Math.atan((Math.sin(yaw)*gx+Math.cos(yaw)*gz)*Math.cos(roll));
      // Large near-vertical placements float or intersect a hillside deeply.
      if(Math.abs(roll)>.65||Math.abs(pitch)>.65)return;
    }
    out.push({module:'MountainRock',params:{shape,seed:rockSeed,referenceSizeM},kind,x:c.x,z:c.z,rotation:c.rot,
      scale:size/referenceSizeM,pitch,roll,groundY,sinkY:size*(shape===1?.08:.16),radius});
  };
  for(const c of candidatesInRect(seed,0xB071,180,ox,oz,sectorSize,sectorSize)) {
    const t=terrain(c.x,c.z);if(!t||t.slope>.32)continue;
    emit(c,16+20*c.u,'landmark',t);
  }
  if(detail<1)return out;
  const radius=38,anchors=[];
  for(const a of candidatesInRect(seed,0xB072,96,ox-radius,oz-radius,sectorSize+2*radius,sectorSize+2*radius)) {
    const t=terrain(a.x,a.z);if(!t)continue;
    const suitability=.25+.65*fade(.025,.24,t.slope);
    if(a.u>suitability)continue;
    anchors.push({...a,radius:18+20*a.v});
  }
  const strength=(x,z)=>{
    let value=0;
    for(const a of anchors) {
      const dx=x-a.x,dz=z-a.z,c=Math.cos(a.rot),s=Math.sin(a.rot);
      const distance=Math.hypot((c*dx+s*dz)/a.radius,(-s*dx+c*dz)/(a.radius*.65));
      value=Math.max(value,1-fade(.2,1,distance));
    }
    return value;
  };
  for(const c of candidatesInRect(seed,0xB073,9,ox,oz,sectorSize,sectorSize)) {
    const cluster=strength(c.x,c.z);
    if(c.u>.025+cluster*.78)continue;
    const t=terrain(c.x,c.z);if(!t)continue;
    emit(c,2+5*fract(c.v*7.31),cluster>.1?'boulder-field':'boulder',t);
  }
  if(detail<2)return out;
  for(const c of candidatesInRect(seed,0xB074,3.8,ox,oz,sectorSize,sectorSize)) {
    const cluster=strength(c.x,c.z);
    if(c.u>.045+cluster*.45)continue;
    const t=terrain(c.x,c.z);if(!t)continue;
    emit(c,.35+1.05*fract(c.v*13.17),'scree',t);
  }
  return out;
}

export function mountainRockClearance(rocks,x,z,radius=0) {
  return rocks.every(p=>p.kind==='scree'||Math.hypot(x-p.x,z-p.z)>p.radius+radius);
}
