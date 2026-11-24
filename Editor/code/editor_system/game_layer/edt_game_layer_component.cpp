#include "edt_game_layer_component.h"
#include "edt_asset_open_component.h"
#include "edt_level_controller.h"
#include "edt_level_load_component.h"
#include "edt_prefab_editor_component.h"
#include <snk/editor/edt_editor_services.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <utility>

edt::game_layer_component::game_layer_component(ports from)
	: m_ports(std::move(from))
	, m_gate([this](bool before) { follow(before); })
{
	if (auto contexts = m_ports.services.ctx_manager.lock()) {
		contexts->set_on_game_layer_change([this](scn::context_manager::game_layer_change change) {
			++m_generation;
			m_gate.on_game_layer_change(change == scn::context_manager::game_layer_change::before);
		});
	}
}

edt::game_layer_component::~game_layer_component() = default;

void edt::game_layer_component::follow(bool before)
{
	if (before) {
		// Nothing of the editor may refer to the old layer once it goes: the set "play", the preview set, the loads in
		// flight, the level. A reload of the module stopped Play already (module_swap_gate) and starts it again.
		// A plain module load or unload (no swap): the person's Play ends, nothing restarts it.
		if (m_ports.stop_play) m_ports.stop_play();
		m_ports.prefab.close_for_layer_change();
		if (m_ports.meshes_before) m_ports.meshes_before();
		m_ports.asset_open.drop_pending();
		m_level_after_change = res::tag{};
		if (m_ports.level_load.in_progress()) {
			m_level_after_change = m_ports.level_load.loading_tag();
			m_ports.level_load.abandon();
		}
		m_ports.level_ctrl.detach_from_game_layer();
		return;
	}
	if (scn::context_set* level = m_ports.services.level_set()) {
		level->set_mode(ecs::run_mode::edit);
	}
	m_ports.level_ctrl.attach_to_game_layer();
	if (m_ports.meshes_after) m_ports.meshes_after();
	if (m_level_after_change.is_valid()) {
		m_ports.level_load.begin(std::exchange(m_level_after_change, res::tag{}));
	}
}
