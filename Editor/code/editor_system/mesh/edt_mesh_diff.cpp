#include "edt_mesh_diff.h"
#include <algorithm>
#include <bit>
#include <cstdint>

namespace edt::mesh
{
	namespace
	{
		// -0 and +0 are equal as numbers and differ in the file's text, so a move from one to the other is a move.
		bool same_bits(const glm::vec3& a, const glm::vec3& b)
		{
			return std::bit_cast<std::uint32_t>(a.x) == std::bit_cast<std::uint32_t>(b.x) && std::bit_cast<std::uint32_t>(a.y) == std::bit_cast<std::uint32_t>(b.y)
				&& std::bit_cast<std::uint32_t>(a.z) == std::bit_cast<std::uint32_t>(b.z);
		}

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
		if (before.flagged_edges() != after.flagged_edges() || !before.same_id_counters(after)) return std::nullopt;

		std::vector<vertex_move> moves;
		for (auto a = lhs_vertices.begin(), b = rhs_vertices.begin(); a != lhs_vertices.end(); ++a, ++b) {
			if (a->first != b->first) return std::nullopt;
			if (!same_bits(a->second.position, b->second.position)) moves.push_back({ a->first, a->second.position, b->second.position });
		}
		for (auto a = before.faces().begin(), b = after.faces().begin(); a != before.faces().end(); ++a, ++b) {
			if (a->first != b->first || !same_face(a->second, b->second)) return std::nullopt;
		}
		return moves;
	}
}
