#version 460 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec4 aColor;

layout (std140, binding = 0) uniform Matrices
{
    mat4 projection;
    mat4 view;
    float time;
    vec3 cameraPosition;
};

out vec4 vColor;

void main()
{
    vColor = aColor;
    gl_Position = projection * view * vec4(aPos, 1.0);
}
