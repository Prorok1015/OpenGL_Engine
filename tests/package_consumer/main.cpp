// Calls into the installed SDK: ds::frame_allocator lives in the engine's common component,
// res::tag is a header-only class from the resource component.
#include "mem_allocator.h"
#include <snk/resource/res_tag.h>

int main()
{
	std::pmr::memory_resource* frame = ds::frame_allocator();
	ds::frame_allocator_reset();

	const res::tag tag = res::tag::make("models/cube.glb");
	const bool ok = frame != nullptr && tag.protocol() == "res" && tag.name() == "cube.glb";
	return ok ? 0 : 1;
}
