# Clay brick source geometry

Eight physical 245 × 84 × 112 mm source bricks, using the engine's bounded
3D solid-field recipe and native GPU mesher. Rounded edges, mould depressions,
deeper dents, numerous small pits and chipped arrises are actual geometry.
The current revision has 60 smaller pockmarks and six short scratches per broad
face; finer material grain is reviewed separately in `ClayBrickMaterialProof`.
Placement is deterministic and uses no physics. The source can also feed the
existing finite-face GPU projector without first generating a mesh.

This is the source-shape review for the layered VT material work. It currently
uses flat per-brick clay colors to expose the geometry. It does **not** yet
demonstrate finite stamp composition into VT, mortar,
paint or moss. Those remain required by the material plan. Do not report these
source meshes as the finished low-poly wall representation.

Native scene: `MATTER_WORLD=ClayBrickGeometryProof`. The group camera is
`0.14 0.20 1.1 0 0.095 0`; inspect the first brick with
`cam -0.31 0.115 0.34 -0.405 0.043 0.025`.
