#include "edt_mesh_transform.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <glm/gtc/matrix_transform.hpp>

namespace edt::mesh
{
	namespace
	{
		bool finite(const glm::vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		// The area-weighted normal of the faces the selection stands for: the selected faces, else the faces that use a selected vertex.
		glm::vec3 selection_normal(const poly_mesh& mesh, const mesh_selection& selection, const std::set<vertex_id>& touched)
		{
			glm::vec3 sum(0.f);
			const bool by_faces = selection.mode() == select_mode::face && !selection.faces().empty();
			for (const auto& [id, f] : mesh.faces()) {
				if (by_faces ? !selection.faces().contains(id) : std::ranges::none_of(f.corners, [&touched](const corner& c) { return touched.contains(c.vertex); })) continue;
				const std::size_t count = f.corners.size();
				for (std::size_t i = 0; i < count; ++i) {
					const vertex* current = mesh.find_vertex(f.corners[i].vertex);
					const vertex* next = mesh.find_vertex(f.corners[(i + 1) % count].vertex);
					if (!current || !next) continue;
					const glm::vec3& a = current->position;
					const glm::vec3& b = next->position;
					sum.x += (a.y - b.y) * (a.z + b.z);
					sum.y += (a.z - b.z) * (a.x + b.x);
					sum.z += (a.x - b.x) * (a.y + b.y);
				}
			}
			return sum;
		}
	}

	std::set<vertex_id> selected_vertices(const poly_mesh& mesh, const mesh_selection& selection)
	{
		std::set<vertex_id> result = selection.touched_vertices(mesh);
		std::erase_if(result, [&mesh](vertex_id id) { return mesh.find_vertex(id) == nullptr; });
		return result;
	}

	std::optional<pivot_frame> selection_frame(const poly_mesh& mesh, const mesh_selection& selection, pivot_mode pivot, gizmo_orientation orientation)
	{
		const std::set<vertex_id> touched = selected_vertices(mesh, selection);
		if (touched.empty()) return std::nullopt;

		pivot_frame frame;
		glm::vec3 sum(0.f);
		glm::vec3 low(std::numeric_limits<float>::max());
		glm::vec3 high(std::numeric_limits<float>::lowest());
		for (vertex_id id : touched) {
			const glm::vec3& p = mesh.find_vertex(id)->position;
			sum += p;
			low = glm::min(low, p);
			high = glm::max(high, p);
		}
		frame.origin = pivot == pivot_mode::median ? sum / static_cast<float>(touched.size()) : (low + high) * 0.5f;
		if (!finite(frame.origin)) frame.origin = glm::vec3(0.f);

		if (orientation == gizmo_orientation::normal) {
			const glm::vec3 normal = selection_normal(mesh, selection, touched);
			const float length = glm::length(normal);
			if (std::isfinite(length) && length > 1e-12f) {
				frame.z = normal / length;
				const glm::vec3 helper = std::abs(frame.z.y) < 0.9f ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(1.f, 0.f, 0.f);
				frame.x = glm::normalize(glm::cross(helper, frame.z));
				frame.y = glm::cross(frame.z, frame.x);
			}
		}
		return frame;
	}

	vertex_snapshot capture_vertices(const poly_mesh& mesh, const std::set<vertex_id>& vertices)
	{
		vertex_snapshot snapshot;
		snapshot.positions.reserve(vertices.size());
		for (vertex_id id : vertices) {
			if (const vertex* v = mesh.find_vertex(id)) snapshot.positions.emplace_back(id, v->position);
		}
		return snapshot;
	}

	std::size_t apply_transform(poly_mesh& mesh, const vertex_snapshot& snapshot, const glm::mat4& transform)
	{
		std::size_t set = 0;
		for (const auto& [id, original] : snapshot.positions) {
			// + 0 turns a -0 into +0: a mirror through zero must not leave a sign in the file that undo cannot give back.
			const glm::vec3 moved = glm::vec3(transform * glm::vec4(original, 1.f)) + glm::vec3(0.f);
			if (!finite(moved)) continue;
			if (mesh.set_position(id, moved)) ++set;
		}
		return set;
	}

	glm::mat4 translation_matrix(const glm::vec3& delta)
	{
		return glm::translate(glm::mat4(1.f), delta);
	}

	glm::mat4 rotation_matrix(const glm::vec3& pivot, const glm::vec3& axis, float radians)
	{
		const float length = glm::length(axis);
		if (!(length > 1e-12f) || !std::isfinite(length) || !std::isfinite(radians)) return glm::mat4(1.f);
		return glm::translate(glm::mat4(1.f), pivot) * glm::rotate(glm::mat4(1.f), radians, axis / length) * glm::translate(glm::mat4(1.f), -pivot);
	}

	glm::mat4 scale_matrix(const pivot_frame& frame, const glm::vec3& factors)
	{
		const glm::mat4 basis(glm::vec4(frame.x, 0.f), glm::vec4(frame.y, 0.f), glm::vec4(frame.z, 0.f), glm::vec4(0.f, 0.f, 0.f, 1.f));
		const glm::mat4 scaling = glm::scale(glm::mat4(1.f), factors);
		return glm::translate(glm::mat4(1.f), frame.origin) * basis * scaling * glm::transpose(basis) * glm::translate(glm::mat4(1.f), -frame.origin);
	}

	float snap_to_step(float value, float step)
	{
		if (!(step > 0.f) || !std::isfinite(step) || !std::isfinite(value)) return value;
		return std::round(value / step) * step;
	}

	glm::vec3 snap_to_step(const glm::vec3& value, float step)
	{
		return glm::vec3(snap_to_step(value.x, step), snap_to_step(value.y, step), snap_to_step(value.z, step));
	}
}
