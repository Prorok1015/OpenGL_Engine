#1161 item 5: wnd::gl_surface (snk/window/wnd_gl_surface.h) becomes rnd::driver::gl_surface (snk/driver/rnd_gl_surface.h,
drv_interface), the same four virtual functions in the same order; the window implements it and links drv_interface, the
GL driver no longer depends on the window. The exported class is renamed: its typeinfo and vtable symbols change, and on
MSVC the import of wnd::window::gl_surface_ptr changes too (the return type is part of the decorated name). Layout and
vtable shape are the same. In the window of 4.0 (#758) this is layout_revision 17, not a major bump: old modules are
refused by the layout key and rebuild.
