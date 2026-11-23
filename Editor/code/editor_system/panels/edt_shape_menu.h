#pragma once
#include "edt_shape_menu_model.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gui { struct theme_semantic_colors; }

namespace edt
{
	using mesh::shape_request;

	// "Add shape" for the scene hierarchy and the prefab editor's tree: the submenu and the "Parametric..." dialog. It makes no node:
	// the owner turns the request into the commands (levels) or an edit of the working copy (prefab editor).
	class shape_menu
	{
	public:
		using on_pick = std::function<void(const shape_request&)>;

		// Inside a menu or popup: the "Add shape" submenu for a shape under `parent` (node ids from the root, empty: the root).
		void draw_menu(const std::vector<std::uint64_t>& parent, const on_pick& pick);
		// Once per frame from the panel's on_render, outside any popup: the dialog the submenu opened.
		void draw_dialog(const gui::theme_semantic_colors& colors, const on_pick& pick);
		// The panel cannot show it (no document, no world): a request for the dialog must not wait for the next frame that can.
		void reset() { m_open_requested = false; }

	private:
		mesh::parametric_shape_form m_form;
		std::vector<std::uint64_t> m_parent;
		bool m_open_requested = false;
		std::string m_problem;
	};
}
