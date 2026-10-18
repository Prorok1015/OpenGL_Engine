#pragma once
#include <snk/common/common.h>
#include "scn_model.h"
#include "scn_mesh_nodes.hpp"
#include "level/scn_level.h"
#include <snk/common/snk_api.h>

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

	SNK_API model_web generate_web(glm::ivec2 size);

	SNK_API model_cube generate_cube();

	SNK_API model_sphere generate_sphere();
}