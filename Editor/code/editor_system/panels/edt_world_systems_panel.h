#pragma once
#include <snk/editor/edt_panel_base.h>
#include "edt_world_systems_model.h"

namespace edt
{
	// View > World Systems (#549): a view of world_systems_model; locked while Play runs. A name that is not
	// registered is marked "unknown" and can only be removed.
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
