// Phase 4 Task 13 — PhysicsPlayground: a World that mixes declarative
// authoring (static roots/entities) with procedural entity generation
// (buildEntities) to exercise RigidBody/BoxCollider components end to end.
class PhysicsPlayground extends World {
  static roots = [
    { module: "PlaygroundFloor", transform: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1] },
  ];

  static entities = [
    {
      id: "floor-body",
      name: "Floor Body",
      components: {
        LocalTransform: { translation: [0, 0, 0] },
        RigidBody: { type: "static" },
        BoxCollider: { halfExtents: [20, 0.1, 20] },
      },
    },
  ];

  buildEntities() {
    for (let i = 0; i < 5; ++i) {
      this.entity({
        id: "crate-" + i,
        name: "Crate " + i,
        components: {
          LocalTransform: { translation: [i * 2 - 4, 5 + i * 2, 0] },
          PartInstance: { part: "Crate" },
          RigidBody: { type: "dynamic" },
          BoxCollider: { halfExtents: [0.5, 0.5, 0.5] },
        },
      });
    }
  }
}
