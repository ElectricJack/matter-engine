import assert from 'node:assert/strict';
import { test } from 'node:test';
import { RecipeBuilder, nodeInput, validateRecipe } from '../../../packages/matter-authoring/index.mjs';
import { recipes, recipeById } from '../src/recipes/index.ts';
import { constructionStages } from '../src/stages.ts';

test('every catalog timeline resolves dependencies before use and preserves the exact final document', () => {
  for (const definition of recipes) {
    const document = definition.build();
    const original = JSON.stringify(document);
    const stages = constructionStages(document);
    assert.equal(stages.at(-1)!.recipe, document);
    for (const stage of stages) {
      const seen = new Set<string>();
      for (const node of stage.recipe.nodes) {
        for (const input of node.inputs) if (input.kind === 'node') assert.ok(seen.has(input.reference), `${definition.id}/${node.id}: dependency ${input.reference}`);
        seen.add(node.id);
      }
      stage.recipe.outputs.forEach(output => assert.ok(seen.has(output)));
      if (stage.recipe.outputs.length) validateRecipe(stage.recipe, { supportedCapabilities: stage.recipe.required_capabilities });
    }
    assert.equal(JSON.stringify(document), original);
  }
});

test('forward references are traversed, CSG operands become the result, unreachable nodes stay out', () => {
  const builder = new RecipeBuilder({ source: { id: 'stage.test', uri: 'test://stage', revision: '1' }, seed: 1n });
  const cut = builder.at({ uri: 'test://stage', line: 1, column: 1 }, () => builder.csg('cut', 'subtract', [nodeInput('body'), nodeInput('tool')]));
  builder.box('body', [2, 2, 2]); builder.sphere('tool', 1, 8); builder.box('unused', [1, 1, 1]);
  const stages = constructionStages(builder.output(cut).build());
  assert.deepEqual(stages.slice(0, -1).map(stage => stage.recipe.nodes.at(-1)!.id), ['body', 'tool', 'cut']);
  assert.deepEqual(stages[1].recipe.outputs, ['body', 'tool']);
  assert.deepEqual(stages[2].recipe.outputs, ['cut']);
  assert.deepEqual(stages[0].recipe.source_map, {});
  stages.forEach(stage => validateRecipe(stage.recipe));
});

test('profile steps do not publish profiles as meshes; repeated final placements exclude the prototype', () => {
  const gearStages = constructionStages(recipeById.get('gear')!.build());
  assert.ok(gearStages[0].description.includes('2D profile'));
  assert.equal(gearStages[0].recipe.outputs.length, 0);
  const chair = recipeById.get('chair')!.build();
  const final = constructionStages(chair).at(-1)!.recipe;
  assert.deepEqual(final.outputs, chair.outputs);
  assert.ok(!final.outputs.includes('slat'));
});
