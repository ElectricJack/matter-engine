import { fileURLToPath } from 'node:url';
import { projectLayout } from '../../../../tools/project_layout.mjs';
export const layout = projectLayout(fileURLToPath(new URL('../../', import.meta.url)));
export const objectFile = (name, scene) => layout.object(name, scene);
