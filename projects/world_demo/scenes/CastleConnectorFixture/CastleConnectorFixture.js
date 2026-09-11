import { defineCastleMaterials } from 'shared-lib/castle_materials';
import {
  connectorCollisionEntities,
  connectorRecipes,
} from 'shared-lib/castle_connector_kit';
import {
  CASTLE_CONNECTOR_FIXTURE,
  connectorFixtureRecords,
} from 'shared-lib/castle_connector_fixture';

const M = defineCastleMaterials('CastleConnectorFixture');
const MATERIALS = {
  stone: M.limestone[1], mortar: M.mortar, floor: M.foundation,
  tile: M.terracotta, timber: M.oak,
};
const OFFSETS = {
  'core-hall-15': [-12, 0, -9],
  'core-hall-30': [-12, 0, 3],
  'core-hall-45': [2, 0, -9],
  'core-hall-neg-30': [2, 0, 3],
};

function transform(translation) {
  return [1, 0, 0, translation[0], 0, 1, 0, translation[1],
    0, 0, 1, translation[2], 0, 0, 0, 1];
}

function translatedEntity(entity, offset) {
  const copy = JSON.parse(JSON.stringify(entity));
  const translation = copy.components.LocalTransform.translation;
  copy.components.LocalTransform.translation = [
    translation[0] + offset[0], translation[1] + offset[1], translation[2] + offset[2],
  ];
  return copy;
}

const RECIPES = connectorRecipes(CASTLE_CONNECTOR_FIXTURE.records, {
  module: 'CastleConnectorFixturePart', materials: MATERIALS, detail: 1.15,
});

class CastleConnectorFixture extends World {
  static camera = { position: [18, 14, 29], target: [7, 1.5, 3] };
  static atmosphere = { groundAlbedo: 0.26 };
  // Inline polygon floors and roof facets live on each root, so expansion must
  // remain disabled even though the masonry and rafters are child parts.
  static roots = RECIPES.map(recipe => ({
    module: recipe.module, params: recipe.params,
    transform: transform(OFFSETS[recipe.recordId]), expand: false,
  }));
  static lights = {
    sun: { dir: [0.35, -0.82, -0.42], color: [1.0, 0.91, 0.78] },
    sky: { color: [0.42, 0.47, 0.56] },
  };

  buildEntities() {
    for (const record of connectorFixtureRecords()) {
      const offset = OFFSETS[record.id];
      for (const entity of connectorCollisionEntities(record,
        { prefix: `fixture:${record.id}` })) this.entity(translatedEntity(entity, offset));
    }
  }
}
