// A small bake catalogue, fitted with instance transforms. Authoring dimensions
// remain the collision/clearance contract; they never become stock asset keys.
import { stoneParams, beamParams, plankParams } from 'shared-lib/castle_primitives';

export const CASTLE_STOCK_SEEDS = 2;
const definitions = {
 CastleStone: { canonical:stoneParams, dimensions:{length:.72,height:.28,depth:.42}, axes:['length','height','depth'] },
 CastleBeam: { canonical:beamParams, dimensions:{length:4,height:.28,width:.24}, axes:['length','height','width'] },
 CastlePlank: { canonical:plankParams, dimensions:{length:2,thickness:.12,width:.28}, axes:['length','thickness','width'] },
};

export function primitiveStock(module,input={}) {
 const definition=definitions[module];
 if(!definition)throw new Error('Unknown castle stock module '+module);
 const desired=definition.canonical(input);
 const params=definition.canonical({...desired,...definition.dimensions,seed:desired.seed%CASTLE_STOCK_SEEDS});
 const scale=definition.axes.map(axis=>desired[axis]/params[axis]);
 return {module,params,scale,transform:[scale[0],0,0,0,0,scale[1],0,0,0,0,scale[2],0,0,0,0,1]};
}

// Postmultiply an existing row-major placement by its local stock fit.
export function fitStockTransform(transform,scale) {
 if(!Array.isArray(transform)||transform.length!==16||!transform.every(Number.isFinite))
  throw new Error('Stock placement requires a finite row-major transform');
 const result=transform.slice();
 for(let row=0;row<4;row++)for(let axis=0;axis<3;axis++)result[row*4+axis]*=scale[axis];
 return result;
}

export function stockPlacement(placement) {
 const stock=primitiveStock(placement.module,placement.params);
 const transform=fitStockTransform(placement.transform??placement.matrix,stock.scale);
 return {...placement,params:stock.params,transform,...(placement.matrix?{matrix:transform}:{} )};
}
