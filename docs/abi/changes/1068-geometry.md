#1068 (PR2): rnd::geometry_desc gains an out-of-line copy constructor, copy assignment and destructor (exports; no move
operations, as before, and no fields: the layout key stays). A geometry read from a binary file keeps that state beside
the object, so a copy made by a module built against the old header (inline copy) loses it and may be written as arrays.
geometry_desc::serialize throws for a geometry read from a binary file. A module that copies a geometry_desc needs an
engine with #1068.
