#1328 (shape casts, EPIC-81 US-81-D, part 3): `scn::shapecast` export (and `scn::spatial_world::shapecast`): a sphere or capsule moved along a direction against the world's triangles. The result type `scn::shapecast_result` (a `scn::cast_status` of clear / hit / unsupported / invalid, and a `scn::shape_hit` for a hit) and the value type `scn::shape_hit` (entity, distance, point, normal, triangle, started_inside) join the layout key; `scn::shape` is the one of part 2. `scn::cast_status` tells a clear path from a shape kind that is not cast (a box, for now) and from arguments that cannot be cast.
layout_revision 31.
`scn::detail::spatial_backend` (private to the scene library, nothing pinned) gains the pure virtual `shapecast`.
A module that calls it needs an engine with this change.
