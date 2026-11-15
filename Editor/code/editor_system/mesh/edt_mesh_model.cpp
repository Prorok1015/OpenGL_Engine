#include "edt_mesh_model.h"
#include <algorithm>
#include <cmath>
#include <format>

namespace edt::mesh
{
	vertex_id poly_mesh::add_vertex(const glm::vec3& position)
	{
		const vertex_id id = m_next_vertex++;
		m_vertices.emplace(id, vertex{ position });
		++m_generation;
		return id;
	}

	bool poly_mesh::corners_are_sound(const std::map<vertex_id, vertex>& vertices, const std::vector<corner>& corners)
	{
		if (corners.size() < 3) {
			return false;
		}
		for (std::size_t i = 0; i < corners.size(); ++i) {
			const corner& current = corners[i];
			const corner& next = corners[(i + 1) % corners.size()];
			if (!vertices.contains(current.vertex) || current.vertex == next.vertex) {
				return false;
			}
		}
		return true;
	}

	face_id poly_mesh::add_face(std::vector<corner> corners, std::uint32_t material_slot)
	{
		if (!corners_are_sound(m_vertices, corners)) {
			return invalid_id;
		}
		const face_id id = m_next_face++;
		m_faces.emplace(id, face{ std::move(corners), material_slot });
		++m_generation;
		return id;
	}

	bool poly_mesh::remove_face(face_id id)
	{
		if (m_faces.erase(id) == 0) {
			return false;
		}
		++m_generation;
		return true;
	}

	bool poly_mesh::remove_vertex(vertex_id id)
	{
		const auto used = std::ranges::any_of(m_faces, [id](const auto& entry) {
			return std::ranges::any_of(entry.second.corners, [id](const corner& c) { return c.vertex == id; });
		});
		if (used || m_vertices.erase(id) == 0) {
			return false;
		}
		std::erase_if(m_edge_flags, [id](const auto& entry) { return entry.first.a == id || entry.first.b == id; });
		++m_generation;
		return true;
	}

	bool poly_mesh::set_position(vertex_id id, const glm::vec3& position)
	{
		const auto it = m_vertices.find(id);
		if (it == m_vertices.end()) {
			return false;
		}
		it->second.position = position;
		++m_generation;
		return true;
	}

	bool poly_mesh::restore_vertex(vertex_id id, const glm::vec3& position)
	{
		if (id == invalid_id || !m_vertices.emplace(id, vertex{ position }).second) {
			return false;
		}
		m_next_vertex = std::max(m_next_vertex, id + 1);
		++m_generation;
		return true;
	}

	bool poly_mesh::restore_face(face_id id, std::vector<corner> corners, std::uint32_t material_slot)
	{
		if (id == invalid_id || m_faces.contains(id) || !corners_are_sound(m_vertices, corners)) {
			return false;
		}
		m_faces.emplace(id, face{ std::move(corners), material_slot });
		m_next_face = std::max(m_next_face, id + 1);
		++m_generation;
		return true;
	}

	const vertex* poly_mesh::find_vertex(vertex_id id) const
	{
		const auto it = m_vertices.find(id);
		return it == m_vertices.end() ? nullptr : &it->second;
	}

	const face* poly_mesh::find_face(face_id id) const
	{
		const auto it = m_faces.find(id);
		return it == m_faces.end() ? nullptr : &it->second;
	}

	void poly_mesh::set_edge_flags(const edge_key& key, edge_flag flags)
	{
		if (flags == edge_flag::none) {
			m_edge_flags.erase(key);
		} else {
			m_edge_flags[key] = flags;
		}
		++m_generation;
	}

	edge_flag poly_mesh::edge_flags(const edge_key& key) const
	{
		const auto it = m_edge_flags.find(key);
		return it == m_edge_flags.end() ? edge_flag::none : it->second;
	}

	std::size_t poly_mesh::triangle_count() const
	{
		std::size_t count = 0;
		for (const auto& entry : m_faces) {
			count += entry.second.corners.size() - 2;
		}
		return count;
	}

	std::vector<std::string> poly_mesh::validate() const
	{
		std::vector<std::string> problems;
		for (const auto& [id, vertex_data] : m_vertices) {
			const glm::vec3& p = vertex_data.position;
			if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
				problems.push_back(std::format("vertex {} has a position that is not finite", id));
			}
		}
		for (const auto& [id, face_data] : m_faces) {
			if (!corners_are_sound(m_vertices, face_data.corners)) {
				problems.push_back(std::format("face {} has fewer than 3 corners, an unknown vertex or a vertex twice in a row", id));
			}
		}
		for (const auto& [key, flags] : m_edge_flags) {
			if (!m_vertices.contains(key.a) || !m_vertices.contains(key.b) || key.a == key.b) {
				problems.push_back(std::format("edge {}-{} is flagged but one of its vertices does not exist", key.a, key.b));
			}
		}
		return problems;
	}
}
