#pragma once
#include <common.h>
#include "scn_model.h"
#include "res_mesh.hpp"

namespace scn
{
	struct vertex
	{
		glm::vec3 position;
		glm::vec3 normal;
		glm::vec2 uv;
	};

	struct model_web
	{
		std::vector<vertex> vertices;
		std::vector<unsigned int> indices;
	};

	struct model_cube
	{
		std::vector<vertex> vertices;
		std::vector<unsigned int> indices;
	};

	struct model_sphere
	{
		std::vector<vertex> vertices;
		std::vector<unsigned int> indices;
	};

	model_web generate_web(glm::ivec2 size);

	model_cube generate_cube();

	model_sphere generate_sphere();
}