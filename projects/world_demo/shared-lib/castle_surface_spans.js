// Rigid physical-stock run planning. Stock modules remain 1/2/4m recipes;
// at most one residual is explicit cut geometry emitted into the owning
// assembly, never a scaled instance or a new globally keyed Part variant.
import { CASTLE_TIMBER_SURFACE_IDS, buildSurfaceShell, buildCutTimberShell, makeSurfaceFrame,
  transformSurfacePoint, placeSurfaceShell, emitSurfaceShell } from 'shared-lib/castle_surface_shells';

export function planTimberSpan(length, placement = {}, family = 'beam') {
  if(!['beam','rafter','post'].includes(family))throw new RangeError('unknown physical timber family');
  if (!Number.isFinite(length) || length < 1e-5)
    throw new RangeError('timber span length must be finite and >=0.00001 metre');
  // A malformed authoring request must not allocate millions of child records.
  if(length>4096)throw new RangeError('timber span exceeds4096m planning budget; split the assembly');
  const frame=makeSurfaceFrame(placement),segments=[];
  let remaining=length,cursor=-length/2;
  const append=(segment)=>{
    const start=cursor,end=cursor+segment.length;
    segments.push({ ...segment, localStart:start,localEnd:end,
      frame:{origin:transformSurfacePoint(frame,[(start+end)/2,0,0]),rotation:[...frame.rotation]},
      start:transformSurfacePoint(frame,[start,0,0]),end:transformSurfacePoint(frame,[end,0,0]),
      // Every stock/cut shell has a closed end at this butt joint. Real
      // load-bearing join design remains the assembly's collision/structure job.
      jointBefore:segments.length?'butt':'free',jointAfter:'butt',
    });
    cursor=end;
  };
  for(const stockLength of [4,2,1]){
    const count=Math.floor(remaining/stockLength);
    for(let i=0;i<count;i++)append({kind:'stock',family,length:stockLength,shellId:`${family}-${stockLength}`,
      module:'CastleBeamSurface',params:{shape:CASTLE_TIMBER_SURFACE_IDS.indexOf(`${family}-${stockLength}`)}});
    remaining-=count*stockLength;
  }
  if(remaining>0){
    // Refuse an unrepresentable sliver explicitly; never silently stretch a
    // preceding stock or drop a remainder. Caller may choose a physical cut
    // layout with fewer stocks when sub-10um precision is intentional.
    if(remaining<1e-5)throw new RangeError('residual cut below0.00001m; author an explicit cut layout');
    append({kind:'cut',family,length:remaining,stockId:`${family}-1`,cutEnd:'+X'});
  }
  segments[segments.length-1].jointAfter='free';
  return {version:1,kind:'timber-span',length,frame,segments};
}

export function spanSegmentShell(segment) {
  const local=segment.kind==='cut' ? buildCutTimberShell(segment.length,segment.family) : buildSurfaceShell(segment.shellId);
  return placeSurfaceShell(local,segment.frame);
}

// CPU assembly emission helper; production may instance stock records and
// emit only residuals inline. Neither form contains a scale transform.
export function emitTimberSpan(part, plan, materials) {
  for(const segment of plan.segments)emitSurfaceShell(part,spanSegmentShell(segment),materials);
}
