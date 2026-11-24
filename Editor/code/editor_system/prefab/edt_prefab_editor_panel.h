#pragma once
#include <snk/editor/edt_panel_base.h>
#include "edt_input_binding.h"
#include "edt_shape_menu.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <snk/scene/level/scn_prefab_desc.h>
#include <glm/glm.hpp>

namespace rnd { class render_system; }
namespace gui { class gui_system; }

namespace edt
{
	class prefab_document;

	class prefab_editor_panel : public panel_base
	{
	public:
		prefab_editor_panel(rnd::render_system& rnd, gui::gui_system& gui);

		// A prefab the window stops showing keeps its preview but gives up the render target the shown one draws into (#1185).
		void set_context(prefab_document* ctx)
		{
			if (m_ctx != ctx) park_camera(m_ctx);
			m_ctx = ctx;
			m_error.clear();
			m_banner_later = false;
		}
		void set_on_apply(std::function<void()> cb) { m_on_apply = std::move(cb); }
		void set_on_revert(std::function<void()> cb) { m_on_revert = std::move(cb); }
		// A node picked in the tree (ids from the root, empty: the root): the editor shows it in the Inspector.
		void set_on_node_selected(std::function<void(const std::vector<std::uint64_t>&)> cb) { m_on_node_selected = std::move(cb); }
		void select_node(const std::vector<std::uint64_t>& path)
		{
			m_selected_path = path;
			m_has_selection = true;
			if (m_on_node_selected) m_on_node_selected(path);
		}
		void clear_selection() { m_selected_path.clear(); m_has_selection = false; }
		void set_on_close(std::function<void()> cb) { m_on_close = std::move(cb); }
		// A prefab that loads in the background (#152): one row of the banner above the content, with a Cancel of its own.
		struct loading_item
		{
			std::uint64_t id = 0;
			std::string text;
		};
		// The loads in progress; none ends the banner. The shown prefab is drawn under it as usual (#1303).
		void set_loadings(std::vector<loading_item> items) { m_loadings = std::move(items); }
		void set_on_cancel_loading(std::function<void(std::uint64_t id)> cb) { m_on_cancel_loading = std::move(cb); }
		bool loading() const { return !m_loadings.empty(); }
		const std::vector<loading_item>& loadings() const { return m_loadings; }
		// What the Cancel button of the load `id` does (tests press it here).
		void cancel_loading(std::uint64_t id) { if (m_on_cancel_loading) m_on_cancel_loading(id); }
		void set_input_router(inp::input_router* router) { m_router = router; }
		// A shape picked in "Add shape" (the request names the parent node); called after the tree is drawn, so the edit may change the working copy.
		void set_on_add_shape(std::function<void(const shape_request&)> cb) { m_on_add_shape = std::move(cb); }

	protected:
		void on_before_begin() override;
		void on_render() override;
		// Closed by its X or the View menu: the preview document closes with it (on_close), as with the Close button.
		void on_hidden() override;

	private:
		void render_toolbar();
		void render_disk_banner();
		void render_tree();
		void render_tree_node(const scn::prefab_desc::prefab_node& node, std::vector<std::uint64_t>& path);
		void render_viewport();
		void render_loading_banner();
		void update_camera_viewport(float width, float height);
		static void park_camera(prefab_document* ctx);

		rnd::render_system& m_rnd;
		gui::gui_system& m_gui;
		prefab_document* m_ctx = nullptr;
		bool m_hide_reported = false;
		std::function<void()> m_on_close;
		std::function<void(std::uint64_t id)> m_on_cancel_loading;
		std::vector<loading_item> m_loadings;
		std::function<void()> m_on_apply;
		std::function<void()> m_on_revert;
		inp::input_router* m_router = nullptr;
		std::function<void(const std::vector<std::uint64_t>&)> m_on_node_selected;
		shape_menu m_shape_menu;
		std::function<void(const shape_request&)> m_on_add_shape;
		std::optional<shape_request> m_pending_shape;
		std::vector<std::uint64_t> m_selected_path;
		bool m_has_selection = false;
		bool m_banner_later = false;
		std::string m_error;
	};
}
