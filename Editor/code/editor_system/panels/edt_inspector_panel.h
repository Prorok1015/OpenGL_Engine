#pragma once
#include "edt_panel_base.h"
#include "level/scn_prefab_desc.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <boost/json/object.hpp>

namespace desc { class desc_system; }
namespace edt  { class component_ui_registry; }

namespace edt
{
	class inspector_panel : public panel_base
	{
	public:
		inspector_panel();

		void set_selected_node(scn::prefab_desc::prefab_node* node, bool readonly = false);
		void set_on_node_changed(std::function<void()> cb);
		void set_component_ui_registry(component_ui_registry* registry);
		void set_desc_system(desc::desc_system* desc_system);

	protected:
		void on_render() override;

	private:
		void rebuild_parent_cache();
		void render_generic_json(boost::json::object& obj);
		void render_generic_json_value(const std::string& key, boost::json::value& val);
		void render_generic_json_readonly(const boost::json::object& obj);
		void render_generic_json_value_readonly(const std::string& key, const boost::json::value& val);
		void draw_add_component_popup();

		scn::prefab_desc::prefab_node* m_selected_node  = nullptr;
		bool                           m_readonly       = false;
		component_ui_registry*         m_ui_registry    = nullptr;
		desc::desc_system*             m_desc_system    = nullptr;
		std::function<void()>          m_on_node_changed;

		// Cached serialized parent_desc data — built once on selection change for read-only display
		std::unordered_map<std::string, boost::json::object> m_parent_cache;
	};
}
