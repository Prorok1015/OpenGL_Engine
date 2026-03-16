#version 460 core

#include "pipline_struct_lib.glsl"
#include "phong_model_lib.frag"

in PiplineStruct PS;

#ifdef USE_NORMAL_MAP
layout(binding = 2) uniform sampler2D normalTxm;
#endif

vec3 getNormalVector()
{
#ifdef USE_NORMAL_MAP
    vec3 normal = texture(normalTxm, PS.UV).rgb;
    normal = normalize(normal * 2.0 - 1.0); // from [0, 1] to [-1, 1]
    return normalize(PS.TBN * normal);
#else
    return normalize(PS.Normal);
#endif
}

out vec4 fragColor;

void main()
{    
    vec3 norm = getNormalVector();
    vec4 result = calculatePhongModel(norm, getMaterialColor(PS.UV), getMaterialSpecular(PS.UV), PS.FragPos);
    if (result.a < 1.0) {
        discard;
    }
    fragColor = result;
}