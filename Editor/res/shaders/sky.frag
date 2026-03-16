#version 420 core
in vec3 TexCoords;
  
layout(binding = 0) uniform samplerCube diffuse;

out vec4 fragColor;

void main()
{    
    fragColor = texture(diffuse, TexCoords);
}