import { makeSurfaceFace, emitSurfaceShell } from 'shared-lib/castle_surface_shells';

class ContactReceiver extends Part {
  static noImpostor = true;
  static lodBudgets = [1];
  static params = { kind: 'wall', ground: 16, rock: 11, wall: 9, excluded: 17 };
  build(p) {
    const faces = [];
    if (p.kind === 'rock') {
      // Analytically placed convex rock: no settling or high-resolution bake.
      const rings = [[-0.12,0.72], [0.25,0.95], [0.85,0.67], [1.13,0.34]];
      const vertices = rings.map(([y,r],j) => Array.from({length:8},(_,i) => {
        const a=i*Math.PI/4+j*0.08;
        return [Math.cos(a)*r, y+(j===1 || j===2 ? 0.055*Math.sin(i*3+j) : 0), Math.sin(a)*r*0.65];
      }));
      for(let j=0;j<3;j++) for(let i=0;i<8;i++) {
        const k=(i+1)%8;
        for(const tri of [[vertices[j][i],vertices[j][k],vertices[j+1][k]],
                          [vertices[j][i],vertices[j+1][k],vertices[j+1][i]]]) {
          const a=tri[1].map((v,k)=>v-tri[0][k]), b=tri[2].map((v,k)=>v-tri[0][k]);
          // The ring winding gives inward cross products; flip the normal.
          const n=[a[2]*b[1]-a[1]*b[2],a[0]*b[2]-a[2]*b[0],a[1]*b[0]-a[0]*b[1]];
          faces.push(makeSurfaceFace(tri,n,`rock-${j}-${i}-${faces.length}`));
        }
      }
      faces.push(makeSurfaceFace(vertices[0],[0,-1,0],'rock-bottom'));
      faces.push(makeSurfaceFace(vertices[3],[0,1,0],'rock-top'));
    } else {
      const shelf=p.kind==='shelf', h=shelf?[0.5,0.06,0.35]:[2.4,1.2,0.2];
      for(let axis=0;axis<3;axis++) for(const sign of [-1,1]) {
        const other=[0,1,2].filter(i=>i!==axis), points=[];
        for(const a of [-1,1]) for(const b of [-1,1]) {
          const v=[0,h[1],0];v[axis]+=sign*h[axis];v[other[0]]+=a*h[other[0]];v[other[1]]+=b*h[other[1]];
          points.push(v);
        }
        const normal=[0,0,0];normal[axis]=sign;
        faces.push(makeSurfaceFace(points,normal,`face-${axis}-${sign}`,
          !shelf && axis===2 && sign===1 ? 'wall' : 'excluded'));
      }
    }
    emitSurfaceShell(this,{faces},{body:p.rock,wall:p.wall,excluded:p.excluded});
  }
}
