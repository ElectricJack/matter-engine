// Small precompiled site records for fresh-runtime Parts. The scalar is URI
// encoded because native PartGraph v1 strings do not support JSON escaping.
// This envelope is independent of the structure operation payload format.
import { validateConnectorRecord } from 'shared-lib/castle_connector_kit';
const SCHEMA='matter.castle-site-surface-record/v1';
export const SITE_SURFACE_RECORD_LIMIT=1024*1024;
function identity(kind,params){
 if(kind!=='connector'&&kind!=='paving')throw new Error('Unknown surface record kind');
 const index=params[kind==='connector'?'connectorIndex':'courtyardIndex']??params.index;
 if(!Number.isInteger(params.siteVariant)||params.siteVariant<0||!Number.isInteger(params.siteSeed)||!Number.isInteger(index)||index<0)
  throw new Error('Invalid surface record identity');
 return {kind,siteVariant:params.siteVariant,siteSeed:params.siteSeed,index};
}
function validate(kind,record){
 if(!record||typeof record.id!=='string'||!record.id)throw new Error('Invalid surface record');
 if(kind==='connector'){
  const result=validateConnectorRecord(record);
  if(!result.valid)throw new Error('Invalid surface connector record: '+result.errors.join('; '));
 }else{
  const p=record.clearPolygon,thickness=record.floor?.thickness??.25;
  if(!Array.isArray(p)||p.length<3||p.length>16||!p.every(v=>Array.isArray(v)&&v.length===2&&v.every(Number.isFinite))||
     !Number.isFinite(record.baseY??0)||!Number.isFinite(thickness)||thickness<.14||
     (record.seed!==undefined&&!Number.isFinite(record.seed)))throw new Error('Invalid surface paving record');
  const area=p.reduce((sum,a,i)=>{const b=p[(i+1)%p.length];return sum+a[0]*b[1]-b[0]*a[1];},0);
  if(!Number.isFinite(area)||Math.abs(area)<1e-9)throw new Error('Invalid surface paving polygon');
  for(let i=0;i<p.length;i++){
   const a=p[i],b=p[(i+1)%p.length];
   for(const c of p)if(Math.sign(area)*((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]))< -1e-7)
    throw new Error('Surface paving polygon must be convex');
  }
 }
}
export function encodeSiteSurfaceRecord(kind,record,params){
 const binding=identity(kind,params);validate(kind,record);
 const payload=encodeURIComponent(JSON.stringify({schema:SCHEMA,...binding,recordId:record.id,record}));
 if(payload.length>SITE_SURFACE_RECORD_LIMIT)throw new RangeError('Surface site record exceeds transport budget');
 return payload;
}
export function decodeSiteSurfaceRecord(kind,params){
 const binding=identity(kind,params),text=params.recordPayload;
 if(typeof text!=='string'||/[^A-Za-z0-9_.!~*'()%\-]/.test(text))throw new Error('Surface site record must be URI encoded');
 if(text.length>SITE_SURFACE_RECORD_LIMIT)throw new RangeError('Surface site record exceeds transport budget');
 const payload=JSON.parse(decodeURIComponent(text));
 if(payload?.schema!==SCHEMA)throw new Error('Unsupported surface site record schema');
 if(Object.keys(binding).some(key=>payload[key]!==binding[key])||payload.recordId!==payload.record?.id)
  throw new Error('Surface site record identity mismatch');
 validate(kind,payload.record);return payload.record;
}
