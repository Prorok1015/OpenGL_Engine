#pragma once
#include "edt_panel_base.h"
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace scn { class context_manager; }

namespace edt
{
	// Read-only viewer of the blackboard of a context set (WORLD_MESSAGING W5): key, value, type and the last
	// writer, sorted by key, with a filter and a picker for the set. Shows what the systems read this frame
	// (the committed values). The rows come from edt_blackboard_view.h.
	class blackboard_panel : public panel_base
	{
	public:
		using manager_getter = std::function<std::shared_ptr<scn::context_manager>()>;

		blackboard_panel();

		// Where the sets come from; the panel shows nothing without it.
		void set_manager_getter(manager_getter getter) { m_manager = std::move(getter); }

	protected:
		void on_render() override;

	private:
		manager_getter m_manager;
		std::string m_set;
		std::array<char, 128> m_filter{};
	};
}
