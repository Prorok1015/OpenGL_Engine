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
#ifdef OPAQUE
out vec4 fragColor;
#else
layout (location = 0) out vec4 fragColor;
layout (location = 1) out float fragWeight;
#endif

void main()
{    
    vec3 norm = getNormalVector();
    vec4 result = calculatePhongModel(norm, getMaterialColor(PS.UV), getMaterialSpecular(PS.UV), PS.FragPos);
    if (result.a < 1.0) {
#ifndef OPAQUE
        vec4 color = result;
        // weight function
        float weight = clamp(pow(min(1.0, color.a * 10.0) + 0.01, 3.0) * 1e8 * 
                             pow(1.0 - gl_FragCoord.z * 0.9, 3.0), 1e-2, 3e3);

        // store pixel color accumulation
        fragColor = vec4(color.rgb * color.a, color.a) * weight;

        // store pixel revealage threshold
        fragWeight = color.a;
#else
       discard;
#endif
    } else {
#ifndef OPAQUE
        discard;
#else
        fragColor = result;
#endif
    }
}