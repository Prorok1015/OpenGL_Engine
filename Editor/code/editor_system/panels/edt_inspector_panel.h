#pragma once
#include "edt_panel_base.h"
#include <snk/scene/level/scn_prefab_desc.h>
#include <functional>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <boost/json/object.hpp>

namespace desc { class desc_system; struct type_schema; }
namespace edt  { class inspector_registry; }
namespace edt::override_edit { class view_cache; }

namespace edt
{
	class inspector_panel : public panel_base
	{
	public:
		// What drew a component in the last frame: the fallback order is custom renderer, schema, generic JSON.
		enum class component_ui : std::uint8_t { none, custom, schema, generic };

		inspector_panel();

		void set_selected_node(scn::prefab_desc::prefab_node* node, bool readonly = false);
		// The selected node is a node of the prefab that `owner` instantiates, `target` the ids from the instance to it.
		// Edits then become override records on `owner`; null owner = the selected node is edited itself.
		void set_override_target(scn::prefab_desc::prefab_node* owner, std::vector<uint64_t> target);
		void set_on_node_changed(std::function<void()> cb);
		void set_inspector_registry(inspector_registry* registry);
		void set_desc_system(desc::desc_system* desc_system);
		// Composed bases of instance nodes are kept here (shared with the hierarchy); null disables override editing.
		void set_view_cache(std::shared_ptr<override_edit::view_cache> cache);
		// The desc under the cache changed: the next frame composes again.
		void invalidate_view();
		component_ui ui_of(const std::string& component_key) const;

	protected:
		void on_render() override;

	private:
		void rebuild_parent_cache();
		bool render_generic_json(boost::json::object& obj);
		bool render_generic_json_value(const std::string& key, boost::json::value& val);
		void commit_if_idle();
		void render_generic_json_readonly(const boost::json::object& obj);
		void render_generic_json_value_readonly(const std::string& key, const boost::json::value& val);
		void draw_add_component_popup();
		void render_override_target();
		// The data the component's parent desc defines (serialized once per selection); null without a ready parent.
		const boost::json::object* parent_json(const std::string& key, const scn::prefab_desc::prefab_comp_node& comp);
		// Schema UI over the component's own overrides: draws parent data + overrides, writes only the changed pointers back.
		bool render_schema_component(const std::string& key, scn::prefab_desc::prefab_comp_node& comp, const desc::type_schema& schema);
		// Schema UI over a full effective component of an instance node; the edit lands in `scratch.overrides`.
		bool render_schema_override(scn::prefab_desc::prefab_comp_node& scratch, const boost::json::object& base_json, const desc::type_schema& schema);

		scn::prefab_desc::prefab_node* m_selected_node  = nullptr;
		bool                           m_readonly       = false;
		scn::prefab_desc::prefab_node* m_owner          = nullptr;
		std::vector<uint64_t>          m_target;
		inspector_registry*            m_inspectors     = nullptr;
		desc::desc_system*             m_desc_system    = nullptr;
		std::shared_ptr<override_edit::view_cache> m_view_cache;
		std::function<void()>          m_on_node_changed;
		// Desc was edited live but not yet pushed to ECS (a drag is still in progress).
		bool                           m_pending_commit = false;

		// Cached serialized parent_desc data — built once on selection change for read-only display
		std::unordered_map<std::string, boost::json::object> m_parent_cache;
		std::unordered_map<std::string, component_ui> m_ui_sources;
	};
}
