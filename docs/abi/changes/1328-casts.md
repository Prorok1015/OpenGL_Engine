#1328 (shape casts, EPIC-81 US-81-D, part 3): `scn::shapecast` export (and `scn::spatial_world::shapecast`): a sphere or capsule moved along a direction against the world's triangles. The value type `scn::shape_hit` (entity, distance, point, normal, triangle, started_inside) joins the layout key; `scn::shape` is the one of part 2.
layout_revision 31.
`scn::detail::spatial_backend` (private to the scene library, nothing pinned) gains the pure virtual `shapecast`.
A module that calls it needs an engine with this change.
