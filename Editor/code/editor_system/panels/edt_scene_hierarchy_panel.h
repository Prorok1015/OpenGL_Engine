#pragma once
#include "edt_panel_base.h"
#include "level/scn_world_desc.h"
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <unordered_set>

namespace edt
{
	class scene_hierarchy_panel : public panel_base
	{
	public:
		scene_hierarchy_panel();

		void set_world_desc(scn::world_desc* desc);
		void set_on_node_selected(std::function<void(scn::prefab_desc::prefab_node*)> cb);
		void set_on_delete_node(std::function<void(const std::string& name)> cb);
		void set_on_delete_nodes(std::function<void(const std::vector<std::string>&)> cb);
		void set_on_create_node(std::function<void(const std::string& type_name)> cb);
		void set_on_rename_node(std::function<void(const std::string& old_name, const std::string& new_name)> cb);
		void set_on_duplicate_node(std::function<void(const std::string& name)> cb);
		void set_on_duplicate_nodes(std::function<void(const std::vector<std::string>&)> cb);
		void set_on_reparent_node(std::function<void(const std::string& node_name, const std::string& new_parent_name, int insert_index)> cb);

		scn::prefab_desc::prefab_node* get_selected_node() const { return m_selected_node; }
		void set_selected_node(scn::prefab_desc::prefab_node* node);

		// Multi-world support
		void set_world_names(std::vector<std::string> names, int active_idx = 0);
		void set_on_world_changed(std::function<void(int)> cb) { m_on_world_changed = std::move(cb); }
		void set_on_create_world(std::function<void(const std::string&)> cb) { m_on_create_world = std::move(cb); }

	protected:
		void on_render() override;

	private:
		const char* get_type_prefix(const scn::prefab_desc::prefab_node& node) const;
		bool node_matches_filter(const scn::prefab_desc::prefab_node& node, const std::string& lower_filter) const;
		void draw_node(scn::prefab_desc::prefab_node& node, const std::string& lower_filter, int& flat_index);
		void draw_prefab_children_readonly(const scn::prefab_desc::prefab_node& node);
		const scn::prefab_desc* get_referenced_prefab(const scn::prefab_desc::prefab_node& node) const;
		void draw_world_tabs();
		void collect_flat_order(const std::vector<scn::prefab_desc::prefab_node>& nodes, const std::string& lower_filter);
		void delete_selected();
		void duplicate_selected();

		scn::world_desc* m_world_desc = nullptr;
		scn::prefab_desc::prefab_node* m_selected_node = nullptr;

		// Multi-selection
		std::unordered_set<scn::prefab_desc::prefab_node*> m_multi_selected;
		std::vector<scn::prefab_desc::prefab_node*>        m_flat_visible_nodes;
		int                                                 m_last_clicked_flat_idx = -1;

		std::function<void(scn::prefab_desc::prefab_node*)>          m_on_node_selected;
		std::function<void(const std::string&)>                      m_on_delete_node;
		std::function<void(const std::vector<std::string>&)>         m_on_delete_nodes;
		std::function<void(const std::string&)>                      m_on_create_node;
		std::function<void(const std::string&, const std::string&)>  m_on_rename_node;
		std::function<void(const std::string&)>                      m_on_duplicate_node;
		std::function<void(const std::vector<std::string>&)>         m_on_duplicate_nodes;
		std::function<void(const std::string&, const std::string&, int)> m_on_reparent_node;
		std::function<void(int)>                                      m_on_world_changed;
		std::function<void(const std::string&)>                      m_on_create_world;

		char m_filter[128]           = {};
		char m_rename_buf[256]       = {};
		scn::prefab_desc::prefab_node* m_rename_node = nullptr;

		// World tabs
		std::vector<std::string> m_world_names;
		int                      m_world_idx        = 0;
		bool                     m_force_tab_switch = false;
		char                     m_new_world_buf[128] = {};
	};
}
