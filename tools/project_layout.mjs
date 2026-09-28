// Node-side authoring tools use the same grouped layout as matter/project_layout.h.
import fs from 'node:fs';
import path from 'node:path';

function entries(directory) {
  if (!fs.existsSync(directory)) return [];
  return fs.readdirSync(directory, { withFileTypes: true })
    .filter(e => !e.name.startsWith('.') && !e.isSymbolicLink())
    .sort((a, b) => a.name.localeCompare(b.name));
}
function insert(index, name, file, kind) {
  if (index.has(name)) throw new Error(`Duplicate ${kind} '${name}': ${index.get(name)} and ${file}`);
  index.set(name, file);
}
export function sceneIndex(project) {
  const index = new Map();
  function visit(directory) {
    for (const entry of entries(directory)) {
      if (!entry.isDirectory() || entry.name === 'objects') continue;
      const folder = path.join(directory, entry.name);
      const script = path.join(folder, entry.name + '.js');
      if (fs.existsSync(script) && fs.statSync(script).isFile()) insert(index, entry.name, folder, 'scene');
      else visit(folder);
    }
  }
  visit(path.join(project, 'scenes'));
  return index;
}
export function objectIndex(root) {
  const index = new Map();
  function visit(directory) {
    for (const entry of entries(directory)) {
      const file = path.join(directory, entry.name);
      if (entry.isDirectory()) visit(file);
      else if (entry.isFile() && entry.name.endsWith('.js')) insert(index, path.basename(file, '.js'), file, 'object');
    }
  }
  visit(root);
  return index;
}
export function projectLayout(project) {
  const scenes = sceneIndex(project);
  const shared = objectIndex(path.join(project, 'objects'));
  const local = new Map();
  return {
    scenes, shared,
    scene(name) {
      const folder = scenes.get(name);
      if (!folder) throw new Error(`Scene '${name}' not found in ${project}`);
      return folder;
    },
    object(module, scene) {
      if (scene && !local.has(scene)) local.set(scene, objectIndex(path.join(this.scene(scene), 'objects')));
      return local.get(scene)?.get(module) ?? shared.get(module);
    },
  };
}
