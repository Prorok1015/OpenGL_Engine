layout (std140, binding = 0) uniform Matrices 
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
layout (std140, binding = 1) uniform GlobalLights
{
#ifdef DIRECTION_LIGHT_COUNT
    DirectionalLight dirlights[DIRECTION_LIGHT_COUNT];
#else
    DirectionalLight dirlights;
#endif
#ifdef POINT_LIGHT_COUNT
    PointLight       pointLights[POINT_LIGHT_COUNT];
#else
    PointLight       pointLights;
#endif
};
#endif

#ifdef USE_TXM_AS_DIFFUSE
layout(binding = 0) uniform sampler2D diffuseTxm;
#endif
#ifdef USE_SPECULAR_MAP
layout(binding = 1) uniform sampler2D specularTxm;
#endif

uniform float shininess;
uniform vec4 diffuseColor;
uniform vec4 emissiveColor;

vec4 getMaterialColor(vec2 uv)
{
#ifdef USE_TXM_AS_DIFFUSE
    return texture(diffuseTxm, uv);
#else
    return diffuseColor;
#endif
}

vec4 getMaterialSpecular(vec2 uv)
{
#ifdef USE_SPECULAR_MAP
    return texture(specularTxm, uv);
#else
    return vec4(0);
#endif
}

vec4 calcDirectionalLight(DirectionalLight light, vec3 norm, vec4 materialColor, vec4 materialSpecular, vec3 viewDir)
{
    vec3 lightDir = normalize(-vec3(light.direction));
    vec3 reflectDir = reflect(-lightDir, norm);  
    float diff = max(dot(norm, lightDir), 0.0);

    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);

    vec4 ambientColor  = light.ambient  * materialColor;
    vec4 diffuseColor  = light.diffuse  * diff * materialColor;  
    vec4 specularColor = light.specular * spec * materialSpecular;
    
    return ambientColor + diffuseColor + specularColor;
}

vec4 calcPointLight(PointLight light, vec3 normal, vec4 materialColor, vec4 materialSpecular, vec3 viewDir, vec3 fragPos)
{
    vec3 lightDir = normalize(light.position.xyz - fragPos);
    // diffuse shading
    float diff = max(dot(normal, lightDir), 0.0);

    // specular shading
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);

    // attenuation
    float dist = length(light.position.xyz - fragPos);
    float attenuation = 1.0 / (light.constant + light.linear * dist + light.quadratic * (dist * dist));   
    
    // combine results
    vec4 ambient  = light.ambient  * materialColor;
    vec4 diffuse  = light.diffuse  * diff * materialColor;
    vec4 specular = light.specular * spec * materialSpecular;
    ambient  *= attenuation;
    diffuse  *= attenuation;
    specular *= attenuation;
    return (ambient + diffuse + specular);
}

vec4 calculatePhongModel(vec3 norm, vec4 materialColor, vec4 materialSpecular, vec3 fragPos)
{
    vec4 result = vec4(0);
#ifdef LIGHTS_ENABLED
    vec3 viewDir = normalize(cameraPosition - fragPos);

    #ifdef DIRECTION_LIGHT_COUNT
    // phase 1: Directional lighting
    for(int i = 0; i < DIRECTION_LIGHT_COUNT; i++)
        result += calcDirectionalLight(dirlights[i], norm, materialColor, materialSpecular, viewDir);
    #endif

    #ifdef POINT_LIGHT_COUNT
    // phase 2: Point lights
    for(int i = 0; i < POINT_LIGHT_COUNT; i++)
        result += calcPointLight(pointLights[i], norm, materialColor, materialSpecular, viewDir, fragPos);
    #endif

    // phase 3: Spot light
    //result += CalcSpotLight(spotLight, norm, FragPos, viewDir);
#else
    result = materialColor;
#endif

    //vec4 gamma = vec4(1/2.2);
    vec4 gamma = vec4(1);
    return pow(result + vec4(emissiveColor.rgb, 0), gamma);
}