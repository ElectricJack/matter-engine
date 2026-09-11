import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const primitiveSource=readFileSync(new URL('../shared-lib/castle_primitives.js',import.meta.url),'utf8');
const primitiveUrl='data:text/javascript;base64,'+Buffer.from(primitiveSource).toString('base64');
const stockSource=readFileSync(new URL('../shared-lib/castle_stock.js',import.meta.url),'utf8')
 .replace("'shared-lib/castle_primitives'",JSON.stringify(primitiveUrl));
const stockUrl='data:text/javascript;base64,'+Buffer.from(stockSource).toString('base64');
const {primitiveStock,fitStockTransform}=await import(stockUrl);
const seen=new Map();
for(let i=0;i<1000;i++)for(const module of ['CastleStone','CastleBeam','CastlePlank']) {
 const input={seed:i,length:.4+i*.007,height:.15+i%5*.04,depth:.2+i%3*.03,width:.14+i%7*.02,thickness:.1+i%3*.03,material:8+i%5};
 const stock=primitiveStock(module,input);
 const axes=module==='CastleStone'?['length','height','depth']:module==='CastleBeam'?['length','height','width']:['length','thickness','width'];
 axes.forEach((axis,index)=>assert.ok(Math.abs(stock.params[axis]*stock.scale[index]-input[axis])<1e-10));
 const keys=seen.get(module)??new Set();keys.add(JSON.stringify(stock.params));seen.set(module,keys);
 // A rotated, translated frame must scale local columns, not its translation.
 const frame=[0,0,1,12,0,1,0,4,-1,0,0,-7,0,0,0,1],fit=fitStockTransform(frame,stock.scale);
 assert.deepEqual([fit[3],fit[7],fit[11]],[12,4,-7]);
 assert.equal(fit[2],stock.scale[2]);assert.equal(fit[8],-stock.scale[0]);
}
for(const [module,keys] of seen)assert.equal(keys.size,10,module+' bounded across arbitrary dimensions');
assert.throws(()=>primitiveStock('Unknown'),/Unknown castle stock/);
console.log('castle stock: PASS — 3,000 dimension fits; ten keys per module across five materials, unchanged frame origins');
