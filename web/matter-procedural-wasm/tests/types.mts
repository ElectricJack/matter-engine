import {createProceduralEngine, transferables, type RecipeInput, type Modifier} from '../dist/index.mjs';
import {RecipeBuilder} from '../dist/authoring/index.mjs';
const b = new RecipeBuilder({source: {id: 'test', uri: 'test://types', revision: '1'}, seed: 1n});
b.output(b.box('box', [1, 1, 1]));
const recipe: RecipeInput = b;
const stack: Modifier[] = [{operation: 'mesh.smooth', iterations: 2}, {operation: 'mesh.simplify', ratio: 0.5}];
const engine = await createProceduralEngine();
const result = await engine.evaluate(recipe, {modifiers: stack, lodRatios: [0.3]});
const arrays: ArrayBuffer[] = transferables(result);
const position: Float32Array = result.meshes[0].positions;
void arrays; void position;
// @ts-expect-error unknown modifier is not supported
const bad: Modifier = {operation: 'retopo'};
void bad;
engine.dispose();
