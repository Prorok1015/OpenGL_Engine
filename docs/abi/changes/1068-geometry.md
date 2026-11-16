#1068 (PR2): rnd::geometry_desc gains an out-of-line default and copy constructor, copy assignment and destructor (exports; no move
operations, as before, and no fields: the layout key stays). A geometry read from a binary file keeps that state beside
the object, so a copy made by a module built against the old header (inline copy) loses it and may be written as arrays.
geometry_desc::serialize throws for a geometry read from a binary file. A module that makes or copies a geometry_desc needs an
engine with #1068. The default constructor drops a state left at its address; serialize also refuses a layout without
vertices. Temporary: in PR5 a member replaces the side table, in the batch that changes the layout key.
