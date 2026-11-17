#include <snk/editor/edt_editor_services.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_scene_layer.h>

scn::context_set* edt::editor_services::level_set() const
{
	const auto contexts = ctx_manager.lock();
	return contexts ? &contexts->level_set() : nullptr;
}

scn::scene_layer* edt::editor_services::active_layer() const
{
	const auto contexts = ctx_manager.lock();
	return contexts ? contexts->game_layer() : nullptr;
}

desc::desc_system* edt::editor_services::descs() const
{
	auto* layer = active_layer();
	return layer ? &layer->descs() : base_descs;
}

res::resource_system* edt::editor_services::resources() const
{
	auto* layer = active_layer();
	return layer ? &layer->resources() : base_resources;
}

res::resource_system* edt::editor_services::writable_resources() const
{
	return base_resources ? base_resources : resources();
}

scn::ecs_assembler* edt::editor_services::assembler() const
{
	auto* layer = active_layer();
	return layer ? &layer->assembler() : base_assembler;
}

ecs::system_factory* edt::editor_services::sfactory() const
{
	auto* layer = active_layer();
	return layer ? &layer->systems() : base_sfactory;
}
