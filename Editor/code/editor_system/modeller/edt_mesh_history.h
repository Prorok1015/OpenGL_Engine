#pragma once
#include "edt_document_history.h"
#include "edt_mesh_diff.h"
#include "edt_mesh_selection.h"
#include "edt_mesh_shapes.h"
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	// What the history's changes act on: the mesh document. A change calls these from its undo() and redo(); the document bakes, bumps its
	// generation and works out its dirtiness in history_applied().
	class mesh_history_target
	{
	public:
		virtual ~mesh_history_target() = default;
		virtual mesh::poly_mesh& history_mesh() = 0;
		// The whole mesh and its generator as they were (a change that is not only a move of vertices).
		virtual void history_replace(const mesh::poly_mesh& mesh, const std::optional<mesh::shape_params>& generator) = 0;
		// The mesh was changed by an undo or a redo; `selection` is what the person had selected at that state of the history.
		virtual void history_applied(const mesh::mesh_selection& selection) = 0;
	};

	// An edit that only moved vertices: the id, the position before and the position after of each. Cheap, and what every gizmo drag is.
	class mesh_positions_change final : public history_change
	{
	public:
		mesh_positions_change(mesh_history_target& target, std::vector<mesh::vertex_move> moves, mesh::mesh_selection before, mesh::mesh_selection after);

		void undo() override;
		void redo() override;
		const std::vector<std::string>& keys() const override;

	private:
		mesh_history_target& m_target;
		std::vector<mesh::vertex_move> m_moves;
		mesh::mesh_selection m_before;
		mesh::mesh_selection m_after;
	};

	// Any other edit: the whole mesh before and after. Costs two copies of the mesh, so the tools that know better (M5 on) record finer
	// changes of their own.
	class mesh_snapshot_change final : public history_change
	{
	public:
		mesh_snapshot_change(mesh_history_target& target, mesh::poly_mesh before, std::optional<mesh::shape_params> generator_before, mesh::poly_mesh after,
			std::optional<mesh::shape_params> generator_after, mesh::mesh_selection selection_before, mesh::mesh_selection selection_after);

		void undo() override;
		void redo() override;
		const std::vector<std::string>& keys() const override;

	private:
		mesh_history_target& m_target;
		mesh::poly_mesh m_before;
		std::optional<mesh::shape_params> m_generator_before;
		mesh::poly_mesh m_after;
		std::optional<mesh::shape_params> m_generator_after;
		mesh::mesh_selection m_selection_before;
		mesh::mesh_selection m_selection_after;
	};
}
