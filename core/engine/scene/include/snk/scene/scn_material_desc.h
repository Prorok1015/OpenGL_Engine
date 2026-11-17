#pragma once
#include <snk/render/rnd_material_desc.h>

namespace scn
{
	// The material description is render's (the renderer consumes it, #236); scene code and files keep naming it here.
	using pass_queue = rnd::pass_queue;
	using material_desc = rnd::material_desc;
}
