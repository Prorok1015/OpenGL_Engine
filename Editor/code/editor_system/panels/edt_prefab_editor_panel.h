#pragma once
#include <snk/editor/edt_panel_base.h>
#include "edt_input_binding.h"
#include <functional>
#include <memory>
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

		void set_context(prefab_document* ctx) { m_ctx = ctx; }
		void set_on_apply(std::function<void()> cb) { m_on_apply = std::move(cb); }
		void set_on_revert(std::function<void()> cb) { m_on_revert = std::move(cb); }
		// A node picked in the tree (ids from the root, empty: the root): the editor shows it in the Inspector.
		void set_on_node_selected(std::function<void(const std::vector<std::uint64_t>&)> cb) { m_on_node_selected = std::move(cb); }
		void clear_selection() { m_selected_path.clear(); m_has_selection = false; }
		void set_on_close(std::function<void()> cb) { m_on_close = std::move(cb); }
		void set_input_router(inp::input_router* router) { m_router = router; }

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
		void update_camera_viewport(float width, float height);

		rnd::render_system& m_rnd;
		gui::gui_system& m_gui;
		prefab_document* m_ctx = nullptr;
		std::function<void()> m_on_close;
		std::function<void()> m_on_apply;
		std::function<void()> m_on_revert;
		inp::input_router* m_router = nullptr;
		std::function<void(const std::vector<std::uint64_t>&)> m_on_node_selected;
		std::vector<std::uint64_t> m_selected_path;
		bool m_has_selection = false;
		bool m_banner_later = false;
		std::string m_error;
	};
}
