# Rock scale proof

Four physical reference sizes of the same weathered boulder: 0.5, 2, 8 and
32 metres. Their instance transforms contain no scale. The geometry is larger;
grain, pits, fissures and POM depth keep their authored metre dimensions.

`shared-lib/mountain_rock_scale_proof.js` provides sample placements and cameras
for equal-distance surface inspection, including grazing angles. Compare lit,
raw material, density, POM-off and native RT views. This checks the size-class
material contract; Streaming Mountains additionally chooses a class and applies
a residual scale between 0.5 and 2. The source remains shared by every instance
of the same shape, seed and class.
