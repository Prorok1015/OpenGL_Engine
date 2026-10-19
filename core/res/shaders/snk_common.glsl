// Declarations every shader of the engine shares.
// The engine's shader preprocessor expands SNK_UBO, SNK_SSBO and SNK_TEX (arguments: set, slot) with the binding table
// of the render backend and injects MAX_LIGHT_COUNT (from rnd::lights_params) and the stage define
// SNK_VERTEX_SHADER, SNK_FRAGMENT_SHADER or SNK_GEOMETRY_SHADER. The C++ side of the blocks is in rnd_shader_manager.h
// (global_params, lights_params) and rnd_bindings.h (object_data).
#ifndef MAX_LIGHT_COUNT
#error MAX_LIGHT_COUNT is defined by the engine shader preprocessor
#endif

// Set 0: frame and view.
SNK_UBO(0, 0) uniform Matrices
{
    mat4 projection;
    mat4 view;
    float time;
    vec3 cameraPosition;
};

struct DirectionalLight
{
    // vec3 have size 16 as vec4. you should remember it.
    vec4 direction;
    vec4 diffuse;
    vec4 ambient;
    vec4 specular;
};

struct PointLight {
    vec4 position;
    vec4 ambient;
    vec4 diffuse;
    vec4 specular;

    float constant;
    float linear;
    float quadratic;
};

#ifdef LIGHTS_ENABLED
// Array lengths are fixed so the std140 offsets never depend on shader variants.
SNK_UBO(0, 1) uniform GlobalLights
{
    DirectionalLight dirlights[MAX_LIGHT_COUNT];
    PointLight       pointLights[MAX_LIGHT_COUNT];
    int              dirLightCount;
    int              pointLightCount;
};
#endif

// Set 3: the object being drawn. The draw's element of the ring is picked by a dynamic offset.
#ifdef SNK_VERTEX_SHADER
SNK_UBO(3, 0) uniform ObjectData
{
    mat4 world;
    uint boneOffset;
};
#endif
