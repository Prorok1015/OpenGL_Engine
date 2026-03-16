#version 460 core
layout (location = 0) in vec3 aPos;

out vec2 uv;

void main()
{
    gl_Position = vec4(aPos.xy, 0, 1);
    uv = (aPos.xy + vec2(1)) / 2;
}