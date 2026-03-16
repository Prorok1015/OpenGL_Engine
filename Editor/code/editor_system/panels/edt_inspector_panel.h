#pragma once
#include "edt_panel_base.h"
#include "level/scn_prefab_desc.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace edt
{
	class inspector_panel : public panel_base
	{
	public:
		using desc_comp_renderer = std::function<bool(scn::prefab_desc::prefab_comp_node&)>;

		inspector_panel();

		void set_selected_node(scn::prefab_desc::prefab_node* node);
		void set_on_node_changed(std::function<void()> cb);

		// Register a renderer for a specific component type_name.
		// Renderer returns true if the component was modified.
		void add_desc_renderer(const std::string& type_name, desc_comp_renderer renderer);

	protected:
		void on_render() override;

	private:
		void render_generic_json(boost::json::object& obj);
		void draw_add_component_popup();

		scn::prefab_desc::prefab_node* m_selected_node = nullptr;
		std::function<void()> m_on_node_changed;
		std::unordered_map<std::string, desc_comp_renderer> m_desc_renderers;
	};
}
