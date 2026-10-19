#version 450 core
in vec3 TexCoords;
  
SNK_TEX(1, 0) uniform samplerCube diffuse;

out vec4 fragColor;

void main()
{    
    fragColor = texture(diffuse, TexCoords);
}