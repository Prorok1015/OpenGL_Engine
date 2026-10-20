#include <snk/editor/edt_editor_services.h>
#include <snk/scene/level/scn_context_manager.h>

scn::context_set* edt::editor_services::level_set() const
{
	const auto contexts = ctx_manager.lock();
	return contexts ? &contexts->level_set() : nullptr;
}
