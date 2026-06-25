// Node cross-check of the pure helpers. lsystem.js is intentionally omitted here
// because it uses a bare `shared-lib/rng` import that node cannot resolve without
// an import map; the C++ reference + the SP-2-guarded bake test cover lsystem.
import { cross, lerp } from '../../shared-lib/vecmath.js';
import { cubic } from '../../shared-lib/bezier.js';
import { ring } from '../../shared-lib/geometry.js';
console.log(JSON.stringify(cross([1,0,0],[0,1,0])));        // [0,0,1]
console.log(cubic([0],[0],[1],[1],0.5)[0]);                  // 0.5
console.log(JSON.stringify(ring(4,1,0).map(p=>p.map(v=>+v.toFixed(6)))));
console.log(JSON.stringify(lerp([0,0,0],[2,4,6],0.5)));      // [1,2,3]
