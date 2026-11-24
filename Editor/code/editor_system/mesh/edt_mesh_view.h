#pragma once
#include "edt_mesh_overlay.h"
#include "edt_mesh_picking.h"
#include "edt_mesh_selection.h"
#include <cstdint>
#include <optional>

namespace edt::mesh
{
	// What the modeller shows and picks of one mesh: the selection mode and the selection, the hover, x-ray, and the adjacency and
	// the picker of the mesh as it is now. Editor state: not in the file, never makes the document dirty. The caches are built
	// when asked for and the mesh has changed since (its generation moved), and the selection loses what the mesh no longer has;
	// a mesh that is replaced (reload, revert) goes through reset(), since two meshes can have the same generation.
	class mesh_view
	{
	public:
		select_mode mode() const { return m_selection.mode(); }
		// Carries the selection over to the new mode (see mesh_selection::set_mode).
		void set_mode(select_mode mode, const poly_mesh& mesh);

		// As it was left by the last call that looked at the mesh; see the class comment.
		const mesh_selection& selection() const { return m_selection; }
		mesh_selection& selection() { return m_selection; }
		// The selection after the caches were brought up to the mesh (what an edit removed is gone from it).
		mesh_selection& current_selection(const poly_mesh& mesh);

		bool xray() const { return m_xray; }
		void set_xray(bool xray) { m_xray = xray; }

		const hover_target& hover() const { return m_hover; }
		void set_hover(const hover_target& hover) { m_hover = hover; }

		const mesh_adjacency& adjacency(const poly_mesh& mesh);
		const mesh_picker& picker(const poly_mesh& mesh);
		// Ready for pick_* / click_select / box_select (its references live as long as this view and `view` do).
		pick_query query(const poly_mesh& mesh, const screen_view& view);

		// A new mesh: nothing selected or hovered and no cache; the mode and x-ray stay as the person set them.
		void reset();

	private:
		void refresh(const poly_mesh& mesh);

		mesh_selection m_selection;
		hover_target m_hover;
		bool m_xray = false;
		std::optional<mesh_adjacency> m_adjacency;
		std::optional<mesh_picker> m_picker;
	};
}
