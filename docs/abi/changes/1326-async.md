#1326 (async batch): `scn::raycast_batch` and `scn::spatial_world::raycast_batch` exports (a batch of rays on the compute lane, answered from a snapshot of the world); value types `scn::ray_request`, `scn::ray_result`, `scn::ray_batch` join the layout key, and `scn::ray_query` is the kind of a request.
layout_revision 27.
A module that calls them needs an engine with this change.
