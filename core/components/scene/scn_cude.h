#pragma once
#if 0
#include <memory>
#include "scn_model.h"
namespace rnd {
	class Shader;
}

void init_cude();
void draw_cude(const rnd::Shader& ourShader);
void term_cude();

scn::Model create_cube();
#endif