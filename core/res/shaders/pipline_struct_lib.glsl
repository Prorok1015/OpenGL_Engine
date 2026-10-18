struct PiplineStruct
{
    vec2 UV;
    vec3 FragPos;
    vec4 Color;
#ifdef USE_NORMAL_MAP
    mat3 TBN;
#else
    vec3 Normal;
#endif
};