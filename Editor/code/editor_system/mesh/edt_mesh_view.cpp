#include "edt_mesh_view.h"

namespace edt::mesh
{
	void mesh_view::refresh(const poly_mesh& mesh)
	{
		if (m_adjacency && m_picker && m_adjacency->is_current(mesh) && m_picker->is_current(mesh)) return;
		m_adjacency.emplace(mesh);
		m_picker.emplace(mesh);
		m_selection.prune(mesh, *m_adjacency);
		m_hover = {};
	}

	void mesh_view::set_mode(select_mode mode, const poly_mesh& mesh)
	{
		refresh(mesh);
		m_selection.set_mode(mode, mesh, *m_adjacency);
		m_hover = {};
	}

	const mesh_adjacency& mesh_view::adjacency(const poly_mesh& mesh)
	{
		refresh(mesh);
		return *m_adjacency;
	}

	const mesh_picker& mesh_view::picker(const poly_mesh& mesh)
	{
		refresh(mesh);
		return *m_picker;
	}

	pick_query mesh_view::query(const poly_mesh& mesh, const screen_view& view)
	{
		refresh(mesh);
		return pick_query{ mesh, *m_adjacency, *m_picker, view, m_xray };
	}

	void mesh_view::reset()
	{
		m_selection = mesh_selection();
		m_hover = {};
		m_adjacency.reset();
		m_picker.reset();
	}
}
