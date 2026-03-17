#pragma once
#include "edt_panel_base.h"
#include "level/scn_prefab_desc.h"
#include <functional>
#include <memory>
#include <string>

namespace desc { class desc_system; }
namespace edt  { class component_ui_registry; }

namespace edt
{
	class inspector_panel : public panel_base
	{
	public:
		inspector_panel();

		void set_selected_node(scn::prefab_desc::prefab_node* node);
		void set_on_node_changed(std::function<void()> cb);
		void set_component_ui_registry(component_ui_registry* registry);
		void set_desc_system(desc::desc_system* desc_system);

	protected:
		void on_render() override;

	private:
		void render_generic_json(boost::json::object& obj);
		void draw_add_component_popup();

		scn::prefab_desc::prefab_node* m_selected_node  = nullptr;
		component_ui_registry*         m_ui_registry    = nullptr;
		desc::desc_system*             m_desc_system    = nullptr;
		std::function<void()>          m_on_node_changed;
	};
}
