// node --experimental-vm-modules tools/export-castle-plans.mjs [output directory]
// Exports actual shared JS plans, including exact portal/stair/void records.
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import {fileURLToPath} from 'node:url';
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const output=path.resolve(process.argv[2]||path.join(repo,'build/qa/castle-grid'));
const context=vm.createContext({});
const modules=new Map();
function get(name){
 if(modules.has(name))return modules.get(name);
 if(!/^castle_[a-z_]+$/.test(name))throw Error('Unsupported shared module '+name);
 const file=path.join(repo,'projects/world_demo/shared-lib',name+'.js');
 const module=new vm.SourceTextModule(fs.readFileSync(file,'utf8'),{context,identifier:file});
 modules.set(name,module);return module;
}
const variants=get('castle_variants');
await variants.link(specifier=>{
 if(!specifier.startsWith('shared-lib/'))throw Error('Unsupported import '+specifier);
 return get(specifier.slice(11));
});
await variants.evaluate();
const compiler=get('castle_plan').namespace;
fs.mkdirSync(output,{recursive:true});
for(const [name,seed] of [['courtyard',9411],['roundkeep',17029],['cloister',28303]]){
 const plan=variants.namespace.castlePlan(name,seed),manifest=compiler.compilePlan(plan);
 fs.writeFileSync(path.join(output,name+'-detailed-plan.json'),JSON.stringify(plan,null,2)+'\n');
 fs.writeFileSync(path.join(output,name+'-detailed-manifest.json'),compiler.planToJSON(manifest)+'\n');
 for(const level of manifest.levels)
  fs.writeFileSync(path.join(output,name+'-'+level.id+'.svg'),compiler.planToSVG(manifest,{levelId:level.id}));
 console.log(name+': '+manifest.roomGraph.reachableRoomIds.length+' reachable rooms, '+manifest.wallModules.length+' wall modules, '+manifest.roofs.length+' roofs');
}
