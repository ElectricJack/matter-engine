// Part-only recipe compilation; neither the constructor nor build() runs.
R"JS(
function __recordSurfaceRecipe(material, builder) {
  if (typeof builder !== 'function')
    throw new TypeError('finiteSurface: appearance and base must be functions');
  const saved = [__surface_ops, __surface_mats, __surface_matlines,
                 __surface_applines, __tape_ops];
  try {
    globalThis.__surface_ops = [];
    globalThis.__surface_mats = [];
    globalThis.__surface_matlines = [];
    globalThis.__surface_applines = [[], [], [], [], [], []];
    const s = __surfaceArg();
    s.source(material, builder(s));
    return [...__surface_ops, ...__surface_mats].join('\n') + '\n';
  } finally {
    [globalThis.__surface_ops, globalThis.__surface_mats,
     globalThis.__surface_matlines, globalThis.__surface_applines,
     globalThis.__tape_ops] = saved;
  }
}
function __compileFiniteSurface(authored, params) {
  const method = authored.finiteSurface;
  if (method === undefined) return undefined;
  if (typeof method !== 'function') throw new TypeError('static finiteSurface must be a function');
  const d = method.call(authored, params);
  return __compileFiniteDescriptor(d, false);
}
function __compilePartSurface(authored, params) {
  const method = authored.surface;
  if (method === undefined) return undefined;
  if (authored.finiteSurface !== undefined)
    throw new TypeError('surface and finiteSurface declarations are mutually exclusive');
  if (typeof method !== 'function') throw new TypeError('static surface must be a function');
  const d = method.call(authored, params);
  if (!d || typeof d !== 'object' || Array.isArray(d) || d.version !== 1 ||
      Reflect.ownKeys(d).some(k => !['version','material','recipe'].includes(k)) ||
      !Number.isInteger(d.material) || d.material < 0 || d.material > 255 ||
      typeof d.recipe !== 'function')
    throw new TypeError('surface requires version:1, material in 0..255 and a recipe function');
  return {version:1, material:d.material, program:__recordSurfaceRecipe(d.material,d.recipe)};
}
function __compileFiniteFrame(f) {
  const result={};
  for(const key of ['originM','u','v','n']) {
    if(!Array.isArray(f[key]) || f[key].length!==3 || !f[key].every(Number.isFinite))
      throw new TypeError('material frame requires finite originM/u/v/n vectors');
    result[key]=f[key].slice();
  }
  return result;
}
function __compileFinitePlacements(placements,sourceCount) {
  if (!Array.isArray(placements) || !placements.length || placements.length>4096)
    throw new RangeError('finiteSurface needs 1..4096 placements');
  const ids=new Set();
  return placements.map(p=>{
    if (!p || Reflect.ownKeys(p).some(k=>!['id','source','matrix'].includes(k)) ||
        typeof p.id!=='string' || !p.id.length || p.id.length>128 || ids.has(p.id) ||
        !Number.isInteger(p.source) || p.source<0 || p.source>=sourceCount ||
        !Array.isArray(p.matrix) || p.matrix.length!==16 || !p.matrix.every(Number.isFinite))
      throw new TypeError('finiteSurface placement requires unique id, source index and finite 4x4 matrix');
    ids.add(p.id);return {id:p.id,source:p.source,matrix:p.matrix.slice()};
  });
}
function __compileFiniteDescriptor(d, leaf) {
  const composite = d && (d.version === 2 || d.version === 3) && !leaf;
  const keys = composite
    ? ['version','boundsMinM','boundsMaxM','pixelM','material','base','sources','placements','periodic', ...(d.version === 3 ? ['receivers'] : [])]
    : ['version','solid','boundsMinM','boundsMaxM','pixelM','material','appearance','base'];
  if (!d || typeof d !== 'object' || Array.isArray(d) ||
      Reflect.ownKeys(d).some(k => !keys.includes(k)) || (!composite && d.version !== 1))
    throw new TypeError('finiteSurface requires a strict version-1 source or version-2/3 composite');
  const material = d.material;
  const result = { version:d.version, boundsMinM:d.boundsMinM,
    boundsMaxM:d.boundsMaxM, pixelM:d.pixelM, material };
  if (!Number.isInteger(material) || material < 0 || material > 255)
    throw new TypeError('finiteSurface material handle must be in 0..255');
  result.baseProgram = __recordSurfaceRecipe(material, d.base);
  if (composite) {
    if (!Array.isArray(d.sources) || d.sources.length < 1 || d.sources.length > 32 ||
        !Array.isArray(d.placements) || d.placements.length < 1 || d.placements.length > 4096)
      throw new RangeError('finiteSurface v2 needs 1..32 sources and 1..4096 placements');
    if (d.version === 3) {
      if (!Array.isArray(d.receivers) || !d.receivers.length || d.receivers.length > 4096)
        throw new RangeError('finiteSurface v3 needs 1..4096 planar receivers');
      result.receivers = d.receivers.map(r => {
        if (!r || Reflect.ownKeys(r).some(k => !['originM','u','v','n','domainM'].includes(k)) ||
            ['originM','u','v','n','domainM'].some(k => !Array.isArray(r[k]) ||
              r[k].length !== (k === 'domainM' ? 4 : 3) || !r[k].every(Number.isFinite)))
          throw new TypeError('finiteSurface receiver requires a finite metric frame and UV domain');
        return r;
      });
    }
    result.sources = d.sources.map(s => __compileFiniteDescriptor(s, true));
    result.placements = __compileFinitePlacements(d.placements,result.sources.length);
    if (d.periodic !== undefined) {
      const p=d.periodic;
      if (!p || Reflect.ownKeys(p).some(k=>!['modules','mappings'].includes(k)) ||
          !Array.isArray(p.modules) || !p.modules.length || p.modules.length>32 ||
          !Array.isArray(p.mappings) || !p.mappings.length || p.mappings.length>4096)
        throw new TypeError('finiteSurface periodic needs 1..32 modules and 1..4096 mappings');
      result.periodic={modules:p.modules.map(m=>{
        if (!m || Reflect.ownKeys(m).some(k=>!['originM','u','v','n','periodM','texelsPerM','placements','base'].includes(k)))
          throw new TypeError('invalid periodic module fields');
        const frame=__compileFiniteFrame(m);
        if (!Array.isArray(m.periodM) || m.periodM.length!==2 || !m.periodM.every(x=>Number.isFinite(x)&&x>0) ||
            !Number.isFinite(m.texelsPerM) || m.texelsPerM<1 || m.texelsPerM>8192)
          throw new RangeError('periodic module requires positive periodM and density 1..8192');
        return {...frame,periodM:m.periodM.slice(),texelsPerM:m.texelsPerM,
          placements:__compileFinitePlacements(m.placements,result.sources.length),
          baseProgram:__recordSurfaceRecipe(material,m.base)};
      }),mappings:p.mappings.map(m=>{
        if (!m || Reflect.ownKeys(m).some(k=>!['module','originM','u','v','n','phase','datumM','uRangeM'].includes(k)) ||
            !Number.isInteger(m.module) || m.module<0 || m.module>=p.modules.length)
          throw new TypeError('invalid periodic receiver mapping');
        const phase=m.phase??[0,0],range=m.uRangeM??[-1e20,1e20],datum=m.datumM??0;
        if (![phase,range].every(a=>Array.isArray(a)&&a.length===2&&a.every(Number.isFinite)) ||
            !(range[1]>range[0]) || !Number.isFinite(datum))
          throw new TypeError('mapping phase, datum and U interval must be finite');
        return {...__compileFiniteFrame(m),module:m.module,phase:phase.slice(),datumM:datum,uRangeM:range.slice()};
      })};
    }
  } else {
    result.solid = d.solid;
    result.appearanceProgram = __recordSurfaceRecipe(material, d.appearance);
  }
  return result;
}
)JS"
