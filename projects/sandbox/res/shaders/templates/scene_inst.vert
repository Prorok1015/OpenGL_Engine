#version 420 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoords;
layout (location = 3) in mat4 model;

layout (std140, binding = 0) uniform Matrices 
{
    mat4 projection;
    mat4 view;
    float time;
    vec3 view_position;
};

out struct PiplineStruct
{
    vec2 UV;
    vec3 Normal;
    vec3 FragPos;
} PS;

void main()
{
    PS.UV = aTexCoords;
    PS.FragPos = vec3(model * vec4(aPos, 1.0));
    PS.Normal = mat3(transpose(inverse(model))) * aNormal;
    gl_Position = projection * view * model * vec4(aPos, 1.0);
}