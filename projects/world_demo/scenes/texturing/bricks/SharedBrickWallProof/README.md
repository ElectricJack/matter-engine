# Shared brick-wall material proof

Three low-poly receivers use the same brick recipe at 8×12, 12×16 and 16×20
bricks/courses. Their local origins agree; their world positions and geometry
are distinct. The source meshes supply material detail through VT and POM.

Inspect `STATSVT`'s `material_pages` and `shared_material_refs` alongside `pool`.
Source-bank reuse alone does not establish shared composed pixels. GPU pool
reservation is a separate measurement. This scene is a validation fixture,
not a claim that arbitrary periodic mapping or sparse weathering is complete.
