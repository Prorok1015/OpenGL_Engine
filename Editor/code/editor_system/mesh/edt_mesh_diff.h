#pragma once
#include "edt_mesh_model.h"
#include <optional>
#include <vector>
#include <glm/glm.hpp>

namespace edt::mesh
{
	struct vertex_move
	{
		vertex_id id = invalid_id;
		glm::vec3 before = glm::vec3(0.f);
		glm::vec3 after = glm::vec3(0.f);
	};

	// What an edit changed when it only moved vertices: the two meshes have the same vertices, faces (corners, UVs, material slots)
	// and edge flags. The moves are ordered by vertex id and empty when nothing moved. nullopt: anything else differs.
	std::optional<std::vector<vertex_move>> moves_between(const poly_mesh& before, const poly_mesh& after);
}
