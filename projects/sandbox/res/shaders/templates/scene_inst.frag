#version 420 core
layout (std140, binding = 0) uniform Matrices 
{
    mat4 projection;
    mat4 view;
    float time;
    vec3 view_position;
};

layout (std140, binding = 1) uniform Light 
{
    // vec3 have size 16 as vec4. you should remember it.
    vec3 position;
    vec3 diffuse;
    vec3 ambient;
    vec3 specular;
} light;

in struct PiplineStruct
{
    vec2 UV;
    vec3 Normal;
    vec3 FragPos;
} PS;


layout(binding = 0) uniform sampler2D diffuse;
layout(binding = 1) uniform sampler2D specular;
layout(binding = 2) uniform sampler2D ambient;
uniform float shininess;
  
out vec4 fragColor;

void main()
{    
    // diffuse
    vec3 norm = normalize(PS.Normal);
    vec3 lightDir = normalize(light.position - PS.FragPos);
    float diff = max(dot(norm, lightDir), 0.0);
    // specular
    vec3 viewDir = normalize(view_position - PS.FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);  
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);

    vec3 ambientColor  = light.ambient  * vec3(texture(diffuse, PS.UV));
    vec3 diffuseColor  = light.diffuse  * diff * vec3(texture(diffuse, PS.UV));  
    vec3 specularColor = light.specular * spec * vec3(texture(specular, PS.UV));

    vec4 result = vec4(ambientColor + diffuseColor + specularColor, 1.0);
    fragColor = texture(diffuse, PS.UV);
}