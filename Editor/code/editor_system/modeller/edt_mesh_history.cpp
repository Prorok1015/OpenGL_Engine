#include "edt_mesh_history.h"
#include <utility>

namespace edt
{
	namespace
	{
		// One key for the whole mesh: a group of an agent's edits (M10) is rolled back as one, and nothing finer is told apart yet.
		const std::vector<std::string>& mesh_keys()
		{
			static const std::vector<std::string> keys{ "mesh" };
			return keys;
		}
	}

	mesh_positions_change::mesh_positions_change(mesh_history_target& target, std::vector<mesh::vertex_move> moves, mesh::mesh_selection before, mesh::mesh_selection after)
		: m_target(target), m_moves(std::move(moves)), m_before(std::move(before)), m_after(std::move(after))
	{
	}

	void mesh_positions_change::undo()
	{
		mesh::poly_mesh& mesh = m_target.history_mesh();
		for (const mesh::vertex_move& move : m_moves) mesh.set_position(move.id, move.before);
		m_target.history_applied(m_before);
	}

	void mesh_positions_change::redo()
	{
		mesh::poly_mesh& mesh = m_target.history_mesh();
		for (const mesh::vertex_move& move : m_moves) mesh.set_position(move.id, move.after);
		m_target.history_applied(m_after);
	}

	const std::vector<std::string>& mesh_positions_change::keys() const
	{
		return mesh_keys();
	}

	mesh_snapshot_change::mesh_snapshot_change(mesh_history_target& target, mesh::poly_mesh before, std::optional<mesh::shape_params> generator_before,
		mesh::poly_mesh after, std::optional<mesh::shape_params> generator_after, mesh::mesh_selection selection_before, mesh::mesh_selection selection_after)
		: m_target(target)
		, m_before(std::move(before))
		, m_generator_before(std::move(generator_before))
		, m_after(std::move(after))
		, m_generator_after(std::move(generator_after))
		, m_selection_before(std::move(selection_before))
		, m_selection_after(std::move(selection_after))
	{
	}

	void mesh_snapshot_change::undo()
	{
		m_target.history_replace(m_before, m_generator_before);
		m_target.history_applied(m_selection_before);
	}

	void mesh_snapshot_change::redo()
	{
		m_target.history_replace(m_after, m_generator_after);
		m_target.history_applied(m_selection_after);
	}

	const std::vector<std::string>& mesh_snapshot_change::keys() const
	{
		return mesh_keys();
	}
}
