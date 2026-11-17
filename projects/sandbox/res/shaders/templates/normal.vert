#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

out VS_OUT {
    vec3 normal;
} vs_out;

#include "snk_common.glsl"

void main()
{
    gl_Position = view * world * vec4(aPos, 1.0); 
    mat3 normalMatrix = mat3(transpose(inverse(view * world)));
    vs_out.normal = normalize(vec3(vec4(normalMatrix * aNormal, 0.0)));
}