#1068 (PR3a): new exports rnd::encode_geometry_files, rnd::write_geometry_files, rnd::read_geometry_file and
rnd::holds_geometry_arrays (snk/render/geom/rnd_geometry_files.h) with the aggregates rnd::geometry_file_options and
rnd::geometry_files, which are not in gs::module_layout_key (a module that builds them needs an engine with #1068; they join
the key in PR5 with the other EPIC-79 types). No existing type changes: geometry_desc keeps its layout, the layout key stays.
Behaviour: geometry_desc_adapter::serialize (a generic store of a geometry_desc resource) now throws; a module that pinned a
geometry_desc to have it written must call rnd::write_geometry_files instead.
