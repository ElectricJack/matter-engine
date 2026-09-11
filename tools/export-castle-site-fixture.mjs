import { mkdir, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { compileSite, siteToJSON, siteToSVG } from '../projects/world_demo/shared-lib/castle_site.js';
import { CASTLE_SITE_ANGLED_STUDY } from '../projects/world_demo/tests/fixtures/castle_site_angled_study.js';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const output = resolve(root, process.argv[2] ?? 'docs/designs/examples');
await mkdir(output, { recursive: true });
const manifest = compileSite(CASTLE_SITE_ANGLED_STUDY);
await Promise.all([
  writeFile(resolve(output, 'castle-site-angled-study.manifest.json'), siteToJSON(manifest)),
  writeFile(resolve(output, 'castle-site-angled-study.svg'), siteToSVG(manifest)),
  writeFile(resolve(output, 'castle-site-angled-study.upper.svg'),
    siteToSVG(manifest, { levelId: 'upper' })),
  writeFile(resolve(output, 'castle-site-angled-study.connector.json'),
    `${JSON.stringify(manifest.connectors[0], null, 2)}\n`),
]);
console.log(`exported castle site fixture to ${output}`);
