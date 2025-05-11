#version 460 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoords;
layout (location = 3) in vec3 aTangent;
layout (location = 4) in vec3 aBitangent;
layout (location = 5) in vec4 aBonesWeight; //TODO: move to texture2D
layout (location = 6) in vec4 aColor;

layout (std140, binding = 0) uniform Matrices 
{
    mat4 projection;
    mat4 view;
    float time;
    vec3 cameraPosition;
};

#include "pipline_struct_lib.glsl"

out PiplineStruct PS;

#include "first_include.vert"
#include "skinning_lib.glsl"

mat4 getMeshMatrix()
{
#ifdef USE_ANIMATION 
    return culcSkinMatrix();
#else
    return uWorldMeshMatr;
#endif
}

mat3 getTBNMatrix(mat4 worldMesh)
{
    vec3 T = normalize(vec3(worldMesh * vec4(aTangent,   0.0)));
    vec3 B = normalize(vec3(worldMesh * vec4(aBitangent, 0.0)));
    vec3 N = normalize(vec3(worldMesh * vec4(aNormal,    0.0)));
    return mat3(T, B, N);
}

void main()
{
    mat4 localMesh = getMeshMatrix();
    mat4 worldMesh = localMesh;

    PS.Color = aColor;
    PS.UV = aTexCoords;
    PS.FragPos = vec3(worldMesh * vec4(aPos, 1.0));
#ifdef USE_NORMAL_MAP
    PS.TBN = getTBNMatrix(worldMesh);
#else
    PS.Normal = abs(normalize(mat3(worldMesh) * aNormal));
#endif

    gl_Position = projection * view * worldMesh * vec4(aPos, 1.0);
}