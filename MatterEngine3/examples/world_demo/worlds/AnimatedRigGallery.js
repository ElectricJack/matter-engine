// Isolated production preview for the procedural animation stack. Entity-
// referenced parts are installed as graph roots but are rendered only through
// the ECS dynamic lane, so this does not duplicate a static manifest instance.
class AnimatedRigGallery extends World {
  // Keep one ordinary manifest instance so the current Vulkan scene path
  // reaches its dynamic-entity reconciliation lane.
  static roots = [{
    module: "Crate",
    transform: [4,0,0,0, 0,0.1,0,-2.5, 0,0,4,0, 0,0,0,1],
  }];

  static entities = [{
    id: "animated-rig-gallery",
    name: "Animated Rig Gallery",
    components: {
      LocalTransform: { translation: [0, 0, 0] },
      PartInstance: { part: "AnimatedRigGallery" },
    },
  }];
}
