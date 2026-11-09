#pragma once
#include "edt_panel_base.h"
#include "edt_world_systems_model.h"

namespace edt
{
	// View > World Systems (#549): the systems of the active world in the order they run within a phase; Add offers
	// the registered ones that are not in the list (a game module's included), a name that is not registered is marked
	// "unknown" and can only be removed. Everything goes through world_systems_model, so it is undone like any edit;
	// locked while Play runs.
	class world_systems_panel final : public panel_base
	{
	public:
		world_systems_panel(editor_events& events, world_systems_model::ports ports);

		world_systems_model& model() { return m_model; }

	protected:
		void on_render() override;

	private:
		world_systems_model m_model;
	};
}
