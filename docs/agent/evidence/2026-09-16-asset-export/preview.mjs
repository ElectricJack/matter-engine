import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import {chromium} from 'playwright';
const [assetFile,outPrefix,format="glb"]=process.argv.slice(2);
if(!assetFile||!outPrefix)throw Error('preview.mjs asset.glb output-prefix');
const root=path.dirname(path.resolve(assetFile));
const html=`<!doctype html><html><head><style>body{margin:0;background:#202830}canvas{display:block}</style><script type="importmap">{"imports":{"three":"/three/build/three.module.js","three/addons/":"/three/examples/jsm/"}}</script></head><body><script type="module">
import * as THREE from 'three';
import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
import {RoomEnvironment} from 'three/addons/environments/RoomEnvironment.js';
import {OBJLoader} from 'three/addons/loaders/OBJLoader.js';
import {MTLLoader} from 'three/addons/loaders/MTLLoader.js';
const renderer=new THREE.WebGLRenderer({antialias:true,preserveDrawingBuffer:true});renderer.setSize(1200,900);renderer.setPixelRatio(1);renderer.outputColorSpace=THREE.SRGBColorSpace;renderer.toneMapping=THREE.ACESFilmicToneMapping;document.body.appendChild(renderer.domElement);
const scene=new THREE.Scene();scene.background=new THREE.Color(0x29333e);const pmrem=new THREE.PMREMGenerator(renderer);scene.environment=pmrem.fromScene(new RoomEnvironment(),.04).texture;
const light=new THREE.DirectionalLight(0xffefdf,3);light.position.set(4,6,8);scene.add(light);scene.add(new THREE.HemisphereLight(0xcde3ff,0x606050,1));
let asset;if('${format}'==='obj'){const manager=new THREE.LoadingManager();const materials=await new MTLLoader(manager).loadAsync('/asset.mtl');const ready=new Promise(resolve=>manager.onLoad=resolve);materials.preload();asset=await new OBJLoader(manager).setMaterials(materials).loadAsync('/asset.obj');await ready;}else{const gltf=await new GLTFLoader().loadAsync('/asset.glb');asset=gltf.scene;}scene.add(asset);const box=new THREE.Box3().setFromObject(asset);const size=box.getSize(new THREE.Vector3()),center=box.getCenter(new THREE.Vector3());
const camera=new THREE.PerspectiveCamera(35,1200/900,.01,1000);const distance=Math.max(size.y,size.x*.75,size.z)*1.9;camera.position.copy(center).add(new THREE.Vector3(.48,.24,1).normalize().multiplyScalar(distance));camera.lookAt(center);
window.report={bounds:{min:box.min.toArray(),max:box.max.toArray()},meshes:[],renderer:renderer.getContext().getParameter(renderer.getContext().RENDERER)};
asset.traverse(o=>{if(o.isMesh)window.report.meshes.push({name:o.name,triangles:(o.geometry.index?.count||o.geometry.attributes.position.count)/3,material:o.material.name,normal:!!o.material.normalMap,uv:!!o.geometry.attributes.uv,tangent:!!o.geometry.attributes.tangent,map:o.material.map?[o.material.map.image.width,o.material.map.image.height]:null});});
window.renderMode=(mode)=>{asset.traverse(o=>{if(!o.isMesh)return;if(!o.userData.pbr)o.userData.pbr=o.material;o.material=mode==='unlit'?new THREE.MeshBasicMaterial({map:o.userData.pbr.map}):o.userData.pbr;});renderer.render(scene,camera);};window.renderMode('pbr');window.ready=true;
</script></body></html>`;
const server=http.createServer((req,res)=>{try{const url=decodeURIComponent(req.url.split('?')[0]);if(url==='/'){res.setHeader('Content-Type','text/html');res.end(html);return;}let file=url.startsWith('/three/')?path.join('/tmp/matter-export-validation/node_modules/three',url.replace(/^\/three\//,'')):path.resolve(root,'.'+url);if(!file.startsWith(root+path.sep)&&!url.startsWith('/three/')){res.writeHead(404);res.end();return;}res.setHeader('Content-Type',file.endsWith('.js')?'text/javascript':'application/octet-stream');fs.createReadStream(file).on('error',()=>res.destroy()).pipe(res);}catch(e){res.writeHead(500);res.end(String(e));}});await new Promise(r=>server.listen(0,'127.0.0.1',r));
const browser=await chromium.launch({headless:true,args:['--no-sandbox','--use-angle=swiftshader','--enable-unsafe-swiftshader']});
try{const page=await browser.newPage({viewport:{width:1200,height:900}});const errors=[];page.on('pageerror',e=>{errors.push(String(e));console.error(String(e));});page.on('console',m=>{if(m.type()==='error'){errors.push(m.text());console.error(m.text());}});await page.goto(`http://127.0.0.1:${server.address().port}/`);await page.waitForFunction(()=>window.ready,null,{timeout:120000});await page.screenshot({path:outPrefix+'-pbr.png'});await page.evaluate(()=>window.renderMode('unlit'));await page.screenshot({path:outPrefix+'-unlit.png'});const report=await page.evaluate(()=>window.report);report.errors=errors;report.viewer='Three.js 0.180.0 '+(format==='obj'?'OBJLoader/MTLLoader':'GLTFLoader')+' / Chromium software WebGL';fs.writeFileSync(outPrefix+'-viewer.json',JSON.stringify(report,null,2));console.log(JSON.stringify(report));if(errors.length)process.exitCode=1;}finally{await browser.close();server.close();}
