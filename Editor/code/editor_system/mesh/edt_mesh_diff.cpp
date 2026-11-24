#include "edt_mesh_diff.h"
#include <algorithm>

namespace edt::mesh
{
	namespace
	{
		bool same_face(const face& lhs, const face& rhs)
		{
			if (lhs.material_slot != rhs.material_slot || lhs.corners.size() != rhs.corners.size()) return false;
			return std::ranges::equal(lhs.corners, rhs.corners, [](const corner& a, const corner& b) { return a.vertex == b.vertex && a.uv == b.uv; });
		}
	}

	std::optional<std::vector<vertex_move>> moves_between(const poly_mesh& before, const poly_mesh& after)
	{
		const auto& lhs_vertices = before.vertices();
		const auto& rhs_vertices = after.vertices();
		if (lhs_vertices.size() != rhs_vertices.size() || before.faces().size() != after.faces().size()) return std::nullopt;
		if (before.flagged_edges() != after.flagged_edges()) return std::nullopt;

		std::vector<vertex_move> moves;
		for (auto a = lhs_vertices.begin(), b = rhs_vertices.begin(); a != lhs_vertices.end(); ++a, ++b) {
			if (a->first != b->first) return std::nullopt;
			if (a->second.position != b->second.position) moves.push_back({ a->first, a->second.position, b->second.position });
		}
		for (auto a = before.faces().begin(), b = after.faces().begin(); a != before.faces().end(); ++a, ++b) {
			if (a->first != b->first || !same_face(a->second, b->second)) return std::nullopt;
		}
		return moves;
	}
}
