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
    const parts = ["Crate", "GlassCrate", "Crate", "GlassCrate", "Crate"];
    for (let i = 0; i < 5; ++i) {
      this.entity({
        id: "crate-" + i,
        name: (parts[i] === "GlassCrate" ? "Glass " : "") + "Crate " + i,
        components: {
          LocalTransform: { translation: [i * 1.5 - 3, 6 + i * 3, (i % 2) * 0.5] },
          PartInstance: { part: parts[i] },
          RigidBody: { type: "dynamic" },
          BoxCollider: { halfExtents: [1.5, 1.5, 1.5] },
        },
      });
    }
    for (let i = 0; i < 4; ++i) {
      this.entity({
        id: "glass-sphere-" + i,
        name: "Glass Sphere " + i,
        components: {
          LocalTransform: { translation: [i * 2.5 - 3.5, 12 + i * 3, 4] },
          PartInstance: { part: "GlassSphere" },
          RigidBody: { type: "dynamic" },
          SphereCollider: { radius: 1.2 },
        },
      });
    }
  }
}
