// BranchGallery — close-up iteration on the branch/foliage look.
//
// objects/BranchGallery.js has existed since the examples/ -> projects/ move
// but had no world file, so it was unreachable from the editor: the only
// gallery of the four (Rock/Tree/Vegetation/Branch) missing its entry point.
// ForestFloor.js's sibling note about Twig.js records the same habit — keep
// the lookdev scene, lose the way in. This restores the way in.
//
// `expand: true` matches the other galleries: the gallery part is a container
// whose children are the thing being looked at, so it publishes its children
// rather than one aggregate part.
class BranchGallery extends World {
  static roots = [{
    module: "BranchGallery",
    transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
    expand: true,
  }];
}
