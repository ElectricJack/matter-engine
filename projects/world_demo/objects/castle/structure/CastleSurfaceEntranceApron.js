import { entranceApronSurfaceShells, emitPavingSurfaceShells } from 'shared-lib/castle_surface_paving';
class CastleSurfaceEntranceApron extends Part {
 static noImpostor=true;
 static lodBudgets=[1];
 static params={x:15,z:40,width:6,depth:2.5,material:8};
 build(p){emitPavingSurfaceShells(this,entranceApronSurfaceShells(p));}
}
