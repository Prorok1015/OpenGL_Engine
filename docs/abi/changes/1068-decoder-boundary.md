#1068 (PR3b): new export rnd::decode_geometry_files (snk/render/geom/rnd_geometry_files.h): the data of a geometry desc and its
binary file held in memory, checked as the loader checks them (res::desc_binary::parse with every hash, then the loader's
data checks), nothing read or stored. No existing type or signature of the engine changes; the layout key stays.
