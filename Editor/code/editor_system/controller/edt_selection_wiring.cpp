#include "edt_selection_wiring.h"
#include "edt_scene_hierarchy_panel.h"
#include "edt_inspector_panel.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include "edt_shared_state.h"
#include <snk/editor/edt_selection_service.h>
#include <memory>

namespace {
	struct flag_scope
	{
		explicit flag_scope(bool& flag) : m_flag(flag) { m_flag = true; }
		~flag_scope() { m_flag = false; }
		bool& m_flag;
	};
}

std::vector<edt::connection> edt::wire_selection(
	editor_events& events,
	selection_service& service,
	shared_state& state,
	scene_editor& scene_ed,
	world_controller& world_ctrl,
	scene_hierarchy_panel& hierarchy,
	inspector_panel& inspector,
	std::function<void(ecs::entity)> set_viewport_entity)
{
	// True while the hierarchy itself is the source of a change: it already holds that state.
	auto from_hierarchy = std::make_shared<bool>(false);

	hierarchy.set_on_node_selected([&service, &state, &hierarchy, from_hierarchy](scn::prefab_desc::prefab_node* node) {
		flag_scope scope(*from_hierarchy);
		if (!node) {
			service.clear();
			return;
		}
		const std::string* world = state.active_world_name();
		selection value;
		value.world = world ? *world : std::string{};
		value.path = hierarchy.get_selected_path();
		value.target = hierarchy.get_selected_target();
		value.readonly = hierarchy.is_selected_readonly();
		service.select(std::move(value));
	});

	scene_ed.on_before_modify = [&service] { service.clear(); };
	scene_ed.on_after_push = [&events, &state] { events.document_changed.emit(document_id{ 0 }, state.desc_generation); };
	world_ctrl.on_world_switched = [&events, &hierarchy, &state](int idx) {
		hierarchy.set_world_desc(state.active_world_desc());
		hierarchy.set_world_names(state.world_names, idx);
		events.active_world_changed.emit(document_id{ 0 });
	};

	std::vector<connection> connections;
	connections.reserve(5);

	// selection_changed: hierarchy, then inspector, then viewport. Each is an independent setter.
	connections.push_back(events.selection_changed.subscribe([&hierarchy, from_hierarchy](const selection& sel) {
		if (sel.empty() && !*from_hierarchy)
			hierarchy.set_selected_node(nullptr);
	}));
	connections.push_back(events.selection_changed.subscribe([&hierarchy, &inspector](const selection&) {
		auto* node = hierarchy.get_selected_node();
		inspector.set_selected_node(node);
		if (node)
			inspector.set_override_target(hierarchy.get_selected_owner(), hierarchy.get_selected_target());
	}));
	connections.push_back(events.selection_changed.subscribe([&service, set_viewport_entity](const selection&) {
		set_viewport_entity(service.entity());
	}));

	// document_changed, i.e. after a push: the inspector's view goes stale before the viewport looks the entity up again.
	connections.push_back(events.document_changed.subscribe([&inspector](document_id, std::uint64_t) {
		inspector.invalidate_view();
	}));
	connections.push_back(events.document_changed.subscribe([&service, set_viewport_entity](document_id, std::uint64_t) {
		set_viewport_entity(service.entity());
	}));
	return connections;
}
