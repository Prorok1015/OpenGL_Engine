#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec4 aColor;

#include "snk_common.glsl"

out vec4 vColor;

void main()
{
    vColor = aColor;
    gl_Position = projection * view * vec4(aPos, 1.0);
}
