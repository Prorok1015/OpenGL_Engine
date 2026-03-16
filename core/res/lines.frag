#version 400 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D texture_diffuse1;

void main(){
    // Parameters
    vec3 circleColor = vec3(0.85, 0.35, 0.2);
//    float thickness = 0.5;
//    // Control with mouse
//    // thickness = iMouse.x / iResolution.x;
//    float fade = 0.005;
//
//    // -1 -> 1 local space, adjusted for aspect ratio
//    vec4 uv = gl_Position.xyza * 2.0 - 1.0;
//    
//    // Calculate distance and fill circle with white
//    float distance = 1.0 - length(uv);
//    vec4 color = vec4(smoothstep(0.0, fade, distance));
//    color *= vec4(smoothstep(thickness + fade, thickness, distance));
//
//    // Set output color
//    FragColor = color;
    FragColor.rgb = circleColor;//texture(texture_diffuse1, TexCoords);
}