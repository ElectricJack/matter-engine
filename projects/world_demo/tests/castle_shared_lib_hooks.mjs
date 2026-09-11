// Node ESM hooks that resolve the engine's flat `shared-lib/<name>` import
// specifiers the way the QuickJS world/part loader does: the project's
// shared-lib shadows MatterEngine3/shared-lib, names are flat, `.js` optional.
// Shared-lib files load as ES modules without a package.json.
//
// Static imports are linked before this module runs, so a test imports this
// module first and then dynamically imports the shared-lib modules under test:
//   await import('./castle_shared_lib_hooks.mjs');
//   const S = await import('../shared-lib/castle_structure.js');
import { register } from 'node:module';

const roots = [
  new URL('../shared-lib/', import.meta.url).href,
  new URL('../../../MatterEngine3/shared-lib/', import.meta.url).href,
];

const hooks = `
import fs from 'node:fs';
let roots = [];
export async function initialize(data) { roots = data.roots; }
export async function resolve(specifier, context, next) {
  if (!specifier.startsWith('shared-lib/')) return next(specifier, context);
  const leaf = specifier.slice('shared-lib/'.length);
  if (!leaf || leaf.includes('/') || leaf.includes('..'))
    throw new Error('shared-lib imports must be flat: ' + specifier);
  const name = leaf.endsWith('.js') ? leaf : leaf + '.js';
  for (const root of roots) {
    const url = new URL(name, root);
    if (fs.existsSync(url)) return { url: url.href, format: 'module', shortCircuit: true };
  }
  throw new Error('shared-lib module not found: ' + specifier);
}
export async function load(url, context, next) {
  if (roots.some((root) => url.startsWith(root)))
    return next(url, { ...context, format: 'module' });
  return next(url, context);
}
`;

register('data:text/javascript,' + encodeURIComponent(hooks), { data: { roots } });
